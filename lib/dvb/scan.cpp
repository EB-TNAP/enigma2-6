// Part 1 Start
#include <fcntl.h>
#include <lib/dvb/idvb.h>
#include <dvbsi++/descriptor_tag.h>
#include <dvbsi++/service_descriptor.h>
#include <dvbsi++/satellite_delivery_system_descriptor.h>
#include <dvbsi++/s2_satellite_delivery_system_descriptor.h>
#include <dvbsi++/terrestrial_delivery_system_descriptor.h>
#include <dvbsi++/t2_delivery_system_descriptor.h>
#include <dvbsi++/cable_delivery_system_descriptor.h>
#include <dvbsi++/logical_channel_descriptor.h>
#include <dvbsi++/ca_identifier_descriptor.h>
#include <dvbsi++/registration_descriptor.h>
#include <dvbsi++/extension_descriptor.h>
#include <dvbsi++/frequency_list_descriptor.h>
#include <lib/base/nconfig.h> // access to python config
#include <lib/dvb/specs.h>
#include <lib/dvb/esection.h>
#include <lib/dvb/scan.h>
#include <lib/dvb/frontend.h>
#include <lib/dvb/db.h>
#include <lib/dvb/frontendparms.h>
#include <lib/base/eenv.h>
#include <lib/base/eerror.h>
#include <lib/base/estring.h>
#include <lib/dvb/dvb.h>
#include <lib/python/python.h>
#include <errno.h>
#include "absdiff.h"



// Define minimum symbol rate supported for DVB-S/S2 (300 symbols/sec)
#define DVB_S_MIN_SYMBOL_RATE 300

#define SCAN_eDebug(x...) do { if (m_scan_debug) eDebug(x); } while(0)
#define SCAN_eDebugNoNewLineStart(x...) do { if (m_scan_debug) eDebugNoNewLineStart(x); } while(0)
#define SCAN_eDebugNoNewLine(x...) do { if (m_scan_debug) eDebugNoNewLine(x); } while(0)

DEFINE_REF(eDVBScan);

eDVBScan::eDVBScan(iDVBChannel *channel, bool usePAT, bool debug)
	:m_channel(channel)
	,m_channel_state(iDVBChannel::state_idle)
	,m_ready(0)
	,m_ready_all(usePAT ? (readySDT|readyPAT) : readySDT)
	,m_pmt_running(false)
	,m_abort_current_pmt(false)
	,m_flags(0)
	,m_networkid(0)
	,m_usePAT(usePAT)
	,m_scan_debug(debug)
	,m_enable_extended_symbolrate(eConfigManager::getConfigBoolValue("config.usage.extended_symbolrate", true))
	,m_tune_timeout_ms(5000)
	,m_scan_progress(0)
	,m_scan_progress_total(0)
	,m_scan_state(scanStateInit)
{
	if (m_channel->getDemux(m_demux))
		SCAN_eDebug("Failed to allocate demux!");
	m_channel->connectStateChange(sigc::mem_fun(*this, &eDVBScan::stateChange), m_stateChanged_connection);
}

eDVBScan::~eDVBScan()
{
	// Ensure clean shutdown
	m_stateChanged_connection.disconnect();
}

int eDVBScan::isValidONIDTSID(int orbital_position, eOriginalNetworkID onid, eTransportStreamID tsid)
{
	// Enhanced validation with more explicit checks
	// Zero ONID is invalid
	if (onid.get() == 0)
		return 0;
		
	// Special case for ONID 1 with TSID < 2 - typically invalid
	if (onid.get() == 1 && tsid < 2)
		return 0;
		
	// Provider-specific ONIDs - typically used for non-standard services
	if (onid.get() >= 0xFF00)
		return 0;
	
	return 1;
}

eDVBNamespace eDVBScan::buildNamespace(eOriginalNetworkID onid, eTransportStreamID tsid, unsigned long hash)
{
	int orb_pos = (hash >> 16) & 0xFFFF;
	
	// Cable networks handling
	if (orb_pos == 0xFFFF) // cable
	{
		if (eConfigManager::getConfigBoolValue("config.usage.subnetwork_cable", true))
			hash &= ~0xFFFF;
	}
	// Terrestrial networks handling
	else if (orb_pos == 0xEEEE) // terrestrial
	{
		if (eConfigManager::getConfigBoolValue("config.usage.subnetwork_terrestrial", true))
			hash &= ~0xFFFF;
	}
	// Satellite networks - use subnetwork option when available
	else if (eConfigManager::getConfigBoolValue("config.usage.subnetwork", true)
		&& isValidONIDTSID(orb_pos, onid, tsid)) // on valid ONIDs, ignore frequency ("sub network") part
		hash &= ~0xFFFF;
	
	return eDVBNamespace(hash);
}

bool eDVBScan::optimizeTuneParameters(ePtr<iDVBFrontendParameters> &feparm)
{
	if (!feparm)
		return false;
		
	int system;
	if (feparm->getSystem(system))
		return false;
		
	// Only optimize for satellite transponders
	if (system != iDVBFrontend::feSatellite)
		return false;
		
	eDVBFrontendParametersSatellite parm;
	if (feparm->getDVBS(parm))
		return false;
	
	bool modified = false;
	
	// Very low symbol rate handling
	if (m_enable_extended_symbolrate && parm.symbol_rate > 0 && parm.symbol_rate < 2000000)
	{
		// For very low SR transponders, set specific parameters
		// that improve the tuning performance
		
		// Always use auto FEC for low SR
		if (parm.fec != eDVBFrontendParametersSatellite::FEC_Auto)
		{
			parm.fec = eDVBFrontendParametersSatellite::FEC_Auto;
			modified = true;
		}
		
		// For DVB-S2 ultralow SR
		if (parm.system == eDVBFrontendParametersSatellite::System_DVB_S2)
		{
			// Auto rolloff and pilot detection helps with unusual configurations
			if (parm.rolloff != eDVBFrontendParametersSatellite::RollOff_auto)
			{
				parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
				modified = true;
			}
			
			if (parm.pilot != eDVBFrontendParametersSatellite::Pilot_Auto)
			{
				parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
				modified = true;
			}
			
			// For ultralow SR (below 1000000), enable longer tuning timeout
			if (parm.symbol_rate < 1000000)
			{
				m_tune_timeout_ms = 8000; // Extended timeout for very low SR
				SCAN_eDebug("Extended timeout for ultralow SR: %d", parm.symbol_rate);
			}
			else
			{
				m_tune_timeout_ms = 5000; // Default timeout
			}
		}
	}
	
	if (modified)
	{
		// Update the parameters with our optimized values
		feparm->setDVBS(parm);
		SCAN_eDebug("Optimized tuning parameters for SR: %d", parm.symbol_rate);
	}
	
	return true;
}

ePtr<iDVBFrontendParameters> eDVBScan::optimizeTransponderParams(iDVBFrontendParameters *tp)
{
	if (!tp)
		return tp;
		
	ePtr<iDVBFrontendParameters> result = tp;
	int system;
	
	if (tp->getSystem(system))
		return result;
		
	// Currently we optimize only satellite parameters
	if (system == iDVBFrontend::feSatellite && m_enable_extended_symbolrate)
	{
		eDVBFrontendParametersSatellite parm;
		if (!tp->getDVBS(parm))
		{
			// Handle very low symbol rate transponders
			if (parm.symbol_rate <= 5000000)
			{
				ePtr<eDVBFrontendParameters> new_feparm = new eDVBFrontendParameters;
				
				// For low SR, always use auto FEC and modulation
				parm.fec = eDVBFrontendParametersSatellite::FEC_Auto;
				
				// For very low SR, adjust system and parameters
				if (parm.symbol_rate <= 1000000)
				{
					// Very low SRs are typically DVB-S2
					if (parm.system != eDVBFrontendParametersSatellite::System_DVB_S2)
					{
						SCAN_eDebug("Very low SR (%d) - switching to DVB-S2", parm.symbol_rate);
						parm.system = eDVBFrontendParametersSatellite::System_DVB_S2;
					}
					
					// Use auto settings for low SR optimization
					parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
					parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
					
					// Low SR usually uses 8PSK or QPSK
					if (parm.modulation == eDVBFrontendParametersSatellite::Modulation_Auto ||
						parm.modulation == eDVBFrontendParametersSatellite::Modulation_QPSK)
					{
						// Leave as is, good for low SR
					}
					else
					{
						// Reset to Auto for better compatibility
						parm.modulation = eDVBFrontendParametersSatellite::Modulation_Auto;
					}
				}
				// For S2 transponders, set auto rolloff/pilot
				else if (parm.system == eDVBFrontendParametersSatellite::System_DVB_S2)
				{
					parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
					parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
				}
				
				new_feparm->setDVBS(parm);
				result = new_feparm;
				
				SCAN_eDebug("Optimized transponder: orbital_pos=%d, freq=%d, SR=%d, pol=%d, sys=%d", 
					parm.orbital_position, parm.frequency, parm.symbol_rate, parm.polarisation, parm.system);
			}
		}
	}
	
	return result;
}

void eDVBScan::stateChange(iDVBChannel *ch)
{
	int state;
	if (ch->getState(state))
		return;
	if (m_channel_state == state)
		return;

	if (state == iDVBChannel::state_ok)
	{
		if (m_ch_current && m_channel)
		{
			int type;
			m_ch_current->getSystem(type);
			if (type == iDVBFrontend::feTerrestrial)
			{
				eDVBFrontendParametersTerrestrial parm;
				m_ch_current->getDVBT(parm);
				if (parm.system == eDVBFrontendParametersTerrestrial::System_DVB_T_T2)
				{
					/* we have a lock, this is a valid DVB-T transponder, set our system parameter */
					parm.system = eDVBFrontendParametersTerrestrial::System_DVB_T;
					m_ch_current->setDVBT(parm);
				}
			}
			// Enhanced handling for DVB-S/S2
			else if (type == iDVBFrontend::feSatellite)
			{
				// If we've locked on a transponder, store additional properties
				ePtr<iDVBFrontend> fe;
				m_channel->getFrontend(fe);
				if (fe)
				{
					eDVBFrontendParametersSatellite parm;
					m_ch_current->getDVBS(parm);
					
					// Check for enhanced parameters from locked transponder
					ePtr<iDVBTransponderData> transponderData;
					if (!fe->getTransponderData(transponderData, false))
					{
						int system = transponderData->getSystem();
						if (system == eDVBFrontendParametersSatellite::System_DVB_S2 && 
							parm.system == eDVBFrontendParametersSatellite::System_DVB_S)
						{
							// Update system for auto-detection between DVB-S and DVB-S2
							parm.system = eDVBFrontendParametersSatellite::System_DVB_S2;
							m_ch_current->setDVBS(parm);
						}
					}
				}
			}
		}
		
		// Blindscan handling
		if (!m_ch_blindscan.empty())
		{
			/* update current blindscan iteration channel with scanned parameters */
			if (m_ch_current && m_channel)
			{
				ePtr<iDVBFrontend> fe;
				m_channel->getFrontend(fe);
				if (fe)
				{
					ePtr<iDVBTransponderData> tp;
					fe->getTransponderData(tp, false);
					if (tp)
					{
						ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
						int type;
						m_ch_current->getSystem(type);
						switch (type)
						{
						case iDVBFrontend::feSatellite:
						{
							eDVBFrontendParametersSatellite parm;
							m_ch_current->getDVBS(parm);
							parm.system = tp->getSystem();
							parm.frequency = tp->getFrequency();
							parm.symbol_rate = tp->getSymbolRate();
							parm.modulation = tp->getModulation();
							
							// Enhanced DVB-S/S2 parameters to make scanning more reliable
							if (tp->getSystem() == eDVBFrontendParametersSatellite::System_DVB_S2)
							{
								parm.rolloff = tp->getRollOff();
								parm.pilot = tp->getPilot();
								
								// For multistream transponders
								parm.is_id = tp->getIsId();
								parm.pls_mode = tp->getPlsMode();
								parm.pls_code = tp->getPlsCode();
							}
							
							feparm->setDVBS(parm);
							break;
						}
						case iDVBFrontend::feCable:
						{
							eDVBFrontendParametersCable parm;
							m_ch_current->getDVBC(parm);
							parm.system = tp->getSystem();
							parm.frequency = tp->getFrequency();
							parm.symbol_rate = tp->getSymbolRate();
							parm.modulation = tp->getModulation();
							feparm->setDVBC(parm);
							break;
						}
						case iDVBFrontend::feTerrestrial:
						{
							eDVBFrontendParametersTerrestrial parm;
							m_ch_current->getDVBT(parm);
							parm.system = tp->getSystem();
							parm.frequency = tp->getFrequency();
							parm.bandwidth = tp->getBandwidth();
							parm.modulation = tp->getConstellation();
							feparm->setDVBT(parm);
							break;
						}
						case iDVBFrontend::feATSC:
						{
							eDVBFrontendParametersATSC parm;
							m_ch_current->getATSC(parm);
							parm.system = tp->getSystem();
							parm.frequency = tp->getFrequency();
							feparm->setATSC(parm);
							break;
						}
						}
						m_ch_current = m_ch_blindscan_result = feparm;
					}
				}
			}
		}
		startFilter();
		m_channel_state = state;
	} 
	else if (state == iDVBChannel::state_failed)
	{
		if (m_ch_current && m_channel)
		{
			int type;
			m_ch_current->getSystem(type);
			m_ch_unavailable.push_back(m_ch_current);
			
			// Enhanced DVB-T/T2 handling
			if (type == iDVBFrontend::feTerrestrial)
			{
				eDVBFrontendParametersTerrestrial parm;
				m_ch_current->getDVBT(parm);
				if (parm.system == eDVBFrontendParametersTerrestrial::System_DVB_T_T2)
				{
					/* we have to scan T2 as well as T */
					ePtr<iDVBFrontend> fe;
					eDVBFrontendParameters eparm;
					parm.system = eDVBFrontendParametersTerrestrial::System_DVB_T2;
					eparm.setDVBT(parm);
					m_channel->getFrontend(fe);
					if (fe)
					{
						ePtr<iDVBFrontendParameters> feparm = new eDVBFrontendParameters(eparm);
						/* but only if the frontend supports T2 */
						if (fe->isCompatibleWith(feparm))
						{
							addChannelToScan(feparm);
						}
					}
				}
			}
			// Enhanced DVB-S/S2 handling for failed locks
			else if (type == iDVBFrontend::feSatellite && m_enable_extended_symbolrate)
			{
				eDVBFrontendParametersSatellite parm;
				m_ch_current->getDVBS(parm);
				
				// For low symbol rate transponders, retry with different parameters
				if (parm.symbol_rate > 0 && parm.symbol_rate <= 5000000)
				{
					// Try with different FEC settings for low SR
					ePtr<iDVBFrontend> fe;
					m_channel->getFrontend(fe);
					if (fe)
					{
						// Create a variant with Auto FEC which can help with unusual FEC configurations
						eDVBFrontendParameters eparm;
						parm.fec = eDVBFrontendParametersSatellite::FEC_Auto;
						
						// For DVB-S2, also try different rolloff and pilot settings
						if (parm.system == eDVBFrontendParametersSatellite::System_DVB_S2)
						{
							parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
							parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
						}
						
						eparm.setDVBS(parm);
						ePtr<iDVBFrontendParameters> feparm = new eDVBFrontendParameters(eparm);
						
						// Only add if the frontend can support this configuration
						if (fe->isCompatibleWith(feparm))
						{
							SCAN_eDebug("Retrying low SR transponder with adjusted parameters: %d", parm.symbol_rate);
							addChannelToScan(feparm);
						}
					}
				}
			}
		}
		
		// Blindscan completion handling
		if (!m_ch_blindscan.empty())
		{
			/* tune failure, this means the blindscan channel iteration run has completed */
			SCAN_eDebug("Blindscan channel completed");
			m_ch_blindscan.pop_front();
		}
		nextChannel();
	}
	/* unavailable will timeout, anyway. */
}

RESULT eDVBScan::nextChannel()
{
	ePtr<iDVBFrontend> fe;

	m_SDT = 0; m_PAT = 0; m_BAT = 0; m_NIT = 0, m_PMT = 0;

	m_ready = 0;

	m_pat_tsid = eTransportStreamID();

	/* check what we need */
	m_ready_all = readySDT;

	if (m_flags & scanNetworkSearch)
		m_ready_all |= readyNIT;

	if (m_flags & scanSearchBAT)
		m_ready_all |= readyBAT;

	if (m_usePAT)
		m_ready_all |= readyPAT;

	if (!m_ch_blindscan.empty())
	{
		/* keep iterating with the same 'channel' till we get a tune failure */
		SCAN_eDebug("Blindscan channel iteration");
		m_ch_current = m_ch_blindscan.front();
	}
	else
	{
		m_ch_blindscan_result = NULL;
		if (m_ch_toScan.empty())
		{
			SCAN_eDebug("No Transponders left: %zd Transponders Scanned, %zd Transponders Unavailable, %zd Transponders in /etc/lamedb.",
				m_ch_scanned.size(), m_ch_unavailable.size(), m_new_channels.size());
			m_event(evtFinish);
			return -ENOENT;
		}

		m_ch_current = m_ch_toScan.front();

		m_ch_toScan.pop_front();
	}

	if (m_channel->getFrontend(fe))
	{
		m_event(evtFail);
		return -ENOTSUP;
	}

	m_chid_current = eDVBChannelID();

	m_channel_state = iDVBChannel::state_idle;

	// Check for low symbol rate transponders
	bool tuningAdjusted = false;
	
	// Get system type to see if this is a satellite transponder
	int system;
	if (!m_ch_current->getSystem(system) && system == iDVBFrontend::feSatellite)
	{
		eDVBFrontendParametersSatellite parm;
		if (!m_ch_current->getDVBS(parm))
		{
			// Optimize tuning parameters if needed
			ePtr<iDVBFrontendParameters> feparm = m_ch_current;
			if (optimizeTuneParameters(feparm))
			{
				// Enhanced tuning for low SR
				if (parm.symbol_rate <= 1000000 && m_enable_extended_symbolrate)
				{
					// Use longer tuning timeout for very low symbol rates
					SCAN_eDebug("Using extended tuning for low SR transponder: %d", parm.symbol_rate);
					if (fe->tune(*feparm, !m_ch_blindscan.empty(), m_tune_timeout_ms))
					{
						return nextChannel();
					}
					tuningAdjusted = true;
				}
			}
		}
	}
	
	// Standard tuning if we didn't do special low SR tuning
	if (!tuningAdjusted && fe->tune(*m_ch_current, !m_ch_blindscan.empty()))
	{
		return nextChannel();
	}

	m_event(evtUpdate);
	return 0;
}

RESULT eDVBScan::startFilter()
{
	bool startSDT=true;
	int system;
	ASSERT(m_demux);

			/* only start required filters filter */

	if (m_ready_all & readyPAT)
		startSDT = m_ready & readyPAT;

	// m_ch_current is not set, when eDVBScan is just used for a SDT update
	if (!m_ch_current)
	{
		unsigned int channelFlags;
		m_channel->getCurrentFrontendParameters(m_ch_current);
		m_ch_current->getFlags(channelFlags);
		if (channelFlags & iDVBFrontendParameters::flagOnlyFree)
			m_flags |= scanOnlyFree;
	}

	m_VCT = 0;
	m_ch_current->getSystem(system);
	if (system == iDVBFrontend::feATSC)
	{
		m_VCT = new eTable<VirtualChannelTableSection>;
		if (m_VCT->start(m_demux, eDVBVCTSpec()))
			return -1;
		CONNECT(m_VCT->tableReady, eDVBScan::VCTready);
		startSDT = false;
	}

	m_SDT = 0;
	if (startSDT && (m_ready_all & readySDT))
	{
		m_SDT = new eTable<ServiceDescriptionSection>;
		int tsid=-1;
		if (m_ready & readyPAT && m_ready & validPAT)
		{
			std::vector<ProgramAssociationSection*>::const_iterator i =
				m_PAT->getSections().begin();
			ASSERT(i != m_PAT->getSections().end());
			tsid = (*i)->getTableIdExtension(); // in PAT this is the transport stream id
			m_pat_tsid = eTransportStreamID(tsid);
			for (; i != m_PAT->getSections().end(); ++i)
			{
				const ProgramAssociationSection &pat = **i;
				ProgramAssociationConstIterator program = pat.getPrograms()->begin();
				for (; program != pat.getPrograms()->end(); ++program)
					m_pmts_to_read.insert(std::pair<unsigned short, service>((*program)->getProgramNumber(), service((*program)->getProgramMapPid())));
			}
			m_PMT = new eTable<ProgramMapSection>;
			CONNECT(m_PMT->tableReady, eDVBScan::PMTready);
			PMTready(-2);
			// KabelBW HACK ... on 618Mhz and 626Mhz the transport stream id in PAT and SDT is different

			{
				int type;
				m_ch_current->getSystem(type);
				if (type == iDVBFrontend::feCable)
				{
					eDVBFrontendParametersCable parm;
					m_ch_current->getDVBC(parm);
					if ((tsid == 0x00d7 && absdiff(parm.frequency, 618000) < 2000) ||
						(tsid == 0x00d8 && absdiff(parm.frequency, 626000) < 2000))
						tsid = -1;
				}
			}
		}
		if (tsid == -1)
		{
			if (m_SDT->start(m_demux, eDVBSDTSpec()))
				return -1;
		}
		else if (m_SDT->start(m_demux, eDVBSDTSpec(tsid, true)))
			return -1;
		CONNECT(m_SDT->tableReady, eDVBScan::SDTready);
	}

	if (!(m_ready & readyPAT))
	{
		m_PAT = 0;
		if (m_ready_all & readyPAT)
		{
			m_PAT = new eTable<ProgramAssociationSection>;
			if (m_PAT->start(m_demux, eDVBPATSpec(4000)))
				return -1;
			CONNECT(m_PAT->tableReady, eDVBScan::PATready);
		}

		m_NIT = 0;
		if (m_ready_all & readyNIT)
		{
			m_NIT = new eTable<NetworkInformationSection>;
			if (m_NIT->start(m_demux, eDVBNITSpec(m_networkid)))
				return -1;
			CONNECT(m_NIT->tableReady, eDVBScan::NITready);
		}

		m_BAT = 0;
		if (m_ready_all & readyBAT)
		{
			m_BAT = new eTable<BouquetAssociationSection>;
			if (m_BAT->start(m_demux, eDVBBATSpec()))
				return -1;
			CONNECT(m_BAT->tableReady, eDVBScan::BATready);
		}
	}
	return 0;
}

// Implementation of the new public methods

int eDVBScan::getScanProgress()
{
	return m_scan_progress;
}

int eDVBScan::getScanProgressTotal()
{
	return m_scan_progress_total;
}

eDVBScan::scanState eDVBScan::getScanState()
{
	return m_scan_state;
}


bool eDVBScan::optimizeTuneParameters(ePtr<iDVBFrontendParameters> &feparm)
{
	if (!feparm)
		return false;
		
	int system;
	if (feparm->getSystem(system))
		return false;
		
	// Only optimize for satellite transponders
	if (system != iDVBFrontend::feSatellite)
		return false;
		
	eDVBFrontendParametersSatellite parm;
	if (feparm->getDVBS(parm))
		return false;
	
	bool modified = false;
	
	// Very low symbol rate handling
	if (m_enable_extended_symbolrate && parm.symbol_rate > 0 && parm.symbol_rate < 2000000)
	{
		// For very low SR transponders, set specific parameters
		// that improve the tuning performance
		
		// Always use auto FEC for low SR
		if (parm.fec != eDVBFrontendParametersSatellite::FEC_Auto)
		{
			parm.fec = eDVBFrontendParametersSatellite::FEC_Auto;
			modified = true;
		}
		
		// For DVB-S2 ultralow SR
		if (parm.system == eDVBFrontendParametersSatellite::System_DVB_S2)
		{
			// Auto rolloff and pilot detection helps with unusual configurations
			if (parm.rolloff != eDVBFrontendParametersSatellite::RollOff_auto)
			{
				parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
				modified = true;
			}
			
			if (parm.pilot != eDVBFrontendParametersSatellite::Pilot_Auto)
			{
				parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
				modified = true;
			}
			
			// For ultralow SR (below 1000000), enable longer tuning timeout
			if (parm.symbol_rate < 1000000)
			{
				m_tune_timeout_ms = 8000; // Extended timeout for very low SR
				SCAN_eDebug("Extended timeout for ultralow SR: %d", parm.symbol_rate);
			}
			else
			{
				m_tune_timeout_ms = 5000; // Default timeout
			}
		}
	}
	
	if (modified)
	{
		// Update the parameters with our optimized values
		feparm->setDVBS(parm);
		SCAN_eDebug("Optimized tuning parameters for SR: %d", parm.symbol_rate);
	}
	
	return true;
}

// End Part 1

// Part 2 start
void eDVBScan::SDTready(int err)
{
	SCAN_eDebug("Got SDT: %d", err);
	m_ready |= readySDT;
	if (!err)
		m_ready |= validSDT;
	channelDone();
}

void eDVBScan::NITready(int err)
{
	SCAN_eDebug("Got NIT: %d", err);
	m_ready |= readyNIT;
	if (!err)
		m_ready |= validNIT;
	channelDone();
}

void eDVBScan::BATready(int err)
{
	SCAN_eDebug("Got BAT: %d", err);
	m_ready |= readyBAT;
	if (!err)
		m_ready |= validBAT;
	channelDone();
}

void eDVBScan::PATready(int err)
{
	SCAN_eDebug("Got PAT: %d", err);
	m_ready |= readyPAT;
	if (!err)
		m_ready |= validPAT;
	startFilter(); // for starting the SDT filter
}

void eDVBScan::VCTready(int err)
{
	SCAN_eDebug("Got VCT: %d", err);
	m_ready |= readySDT;
	if (!err)
		m_ready |= validVCT;
	channelDone();
}

void eDVBScan::PMTready(int err)
{
	if (m_scan_debug && err != -2) // Don't log initial PMT call
		SCAN_eDebug("Got PMT: %d", err);
	
	if (!err)
	{
		bool scrambled = false;
		bool have_audio = false;
		bool have_video = false;
		unsigned short pcrpid = 0xFFFF;
		std::vector<ProgramMapSection*>::const_iterator i;

		for (i = m_PMT->getSections().begin(); i != m_PMT->getSections().end(); ++i)
		{
			const ProgramMapSection &pmt = **i;
			if (pcrpid == 0xFFFF)
				pcrpid = pmt.getPcrPid();
			else
				SCAN_eDebug("Already have a pcrpid %04x %04x", pcrpid, pmt.getPcrPid());
				
			ElementaryStreamInfoConstIterator es;
			for (es = pmt.getEsInfo()->begin(); es != pmt.getEsInfo()->end(); ++es)
			{
				int isaudio = 0, isvideo = 0, is_scrambled = 0, forced_audio = 0, forced_video = 0;
				switch ((*es)->getType())
				{
				case 0x1b: // AVC Video Stream (MPEG4 H264)
				case 0x24: // H265 HEVC
				case 0x10: // MPEG 4 Part 2
				case 0x01: // MPEG 1 video
				case 0x02: // MPEG 2 video
					isvideo = 1;
					forced_video = 1;
					[[fallthrough]];
				case 0x03: // MPEG 1 audio
				case 0x04: // MPEG 2 audio
				case 0x0f: // MPEG 2 AAC
				case 0x11: // MPEG 4 AAC
				case 0x06: // AC3 (private PES)
				case 0x81: // ATSC AC3
				case 0x87: // ATSC Enhanced AC3
					if (!isvideo)
					{
						if ((*es)->getType() >= 0x03 && (*es)->getType() <= 0x11)
						{
							forced_audio = 1;
							isaudio = 1;
						}
					}
					[[fallthrough]];
				case 0x05: // Private Section
				case 0x06: // PES Private
				case 0x81: // user private
				case 0xEA: // TS_PSI_ST_SMPTE_VC1
					for (DescriptorConstIterator desc = (*es)->getDescriptors()->begin();
							desc != (*es)->getDescriptors()->end(); ++desc)
					{
						uint8_t tag = (*desc)->getTag();
						/* PES private can contain AC-3, DTS or lots of other stuff.
						   check descriptors to get the exakt type. */
						if (!forced_video && !forced_audio)
						{
							switch (tag)
							{
							case 0x1C: // TS_PSI_DT_MPEG4_Audio
							case 0x2B: // TS_PSI_DT_MPEG2_AAC
							case AAC_DESCRIPTOR:
							case AC3_DESCRIPTOR:
							case DTS_DESCRIPTOR:
							case AUDIO_STREAM_DESCRIPTOR:
								isaudio = 1;
								break;
							case 0x28: // TS_PSI_DT_AVC
							case 0x1B: // TS_PSI_DT_MPEG4_Video
							case VIDEO_STREAM_DESCRIPTOR:
								isvideo = 1;
								break;
							case REGISTRATION_DESCRIPTOR: /* some services don't have a separate AC3 descriptor */
							{
								RegistrationDescriptor *d = (RegistrationDescriptor*)(*desc);
								switch (d->getFormatIdentifier())
								{
								case 0x44545331 ... 0x44545333: // DTS1/DTS2/DTS3
								case 0x41432d33: // == 'AC-3'
								case 0x42535344: // == 'BSSD' (LPCM)
									isaudio = 1;
									break;
								case 0x56432d31: // == 'VC-1'
									isvideo = 1;
									break;
								default:
									break;
								}
								break; // Add missing break to prevent fallthrough
							}
							default:
								break;
							}
						}
						if (tag == CA_DESCRIPTOR)
							is_scrambled = 1;
					}
				default:
					break;
				}
				if (isvideo)
					have_video = true;
				else if (isaudio)
					have_audio = true;
				else
					continue;
				if (is_scrambled)
					scrambled = true;
			}
			for (DescriptorConstIterator desc = pmt.getDescriptors()->begin();
				desc != pmt.getDescriptors()->end(); ++desc)
			{
				if ((*desc)->getTag() == CA_DESCRIPTOR)
					scrambled = true;
			}
		}
		
		// Save PMT results
		m_pmt_in_progress->second.scrambled = scrambled;
		
		// Enhanced service type detection
		if (have_video)
		{
			// Check for specific video services (SD/HD/UHD)
			m_pmt_in_progress->second.serviceType = 1; // Default to TV
			
			// Process service type based on PMT data
			// This could be enhanced more with actual resolution detection
			for (i = m_PMT->getSections().begin(); i != m_PMT->getSections().end(); ++i)
			{
				const ProgramMapSection &pmt = **i;
				for (DescriptorConstIterator desc = pmt.getDescriptors()->begin();
					desc != pmt.getDescriptors()->end(); ++desc)
				{
					// Check for service type overrides
					// For future enhancement
				}
			}
		}
		else if (have_audio)
		{
			m_pmt_in_progress->second.serviceType = 2; // Radio
		}
		else
		{
			m_pmt_in_progress->second.serviceType = 100; // Data
		}
	}
	
	// Handle PMT processing flow
	if (err == -1) // timeout or removed by sdt
		m_pmts_to_read.erase(m_pmt_in_progress++);
	else if (m_pmt_running)
		++m_pmt_in_progress;
	else
	{
		m_pmt_in_progress = m_pmts_to_read.begin();
		m_pmt_running = true;
	}

	// Start next PMT or finish
	if (m_pmt_in_progress != m_pmts_to_read.end())
	{
		m_PMT->start(m_demux, eDVBPMTSpec(m_pmt_in_progress->second.pmtPid, m_pmt_in_progress->first, 4000));
	}
	else
	{
		m_PMT = 0;
		m_pmt_running = false;
		channelDone();
	}
}

void eDVBScan::addKnownGoodChannel(const eDVBChannelID &chid, iDVBFrontendParameters *feparm)
{
	/* add it to the list of known channels. */
	if (chid)
	{
		m_new_channels.insert(std::pair<eDVBChannelID,ePtr<iDVBFrontendParameters> >(chid, feparm));
		
		// Enhanced tracking for scan progress
		if (m_scan_debug)
		{
			int system;
			if (!feparm->getSystem(system))
			{
				switch(system)
				{
				case iDVBFrontend::feSatellite:
				{
					eDVBFrontendParametersSatellite parm;
					feparm->getDVBS(parm);
					SCAN_eDebug("Added good satellite channel: Orbital %d, Frequency %d, SR %d, Pol %d",
						parm.orbital_position, parm.frequency, parm.symbol_rate, parm.polarisation);
					break;
				}
				case iDVBFrontend::feCable:
				{
					eDVBFrontendParametersCable parm;
					feparm->getDVBC(parm);
					SCAN_eDebug("Added good cable channel: Frequency %d, SR %d", 
						parm.frequency, parm.symbol_rate);
					break;
				}
				case iDVBFrontend::feTerrestrial:
				{
					eDVBFrontendParametersTerrestrial parm;
					feparm->getDVBT(parm);
					SCAN_eDebug("Added good terrestrial channel: Frequency %d, Bandwidth %d",
						parm.frequency, parm.bandwidth);
					break;
				}
				case iDVBFrontend::feATSC:
				{
					eDVBFrontendParametersATSC parm;
					feparm->getATSC(parm);
					SCAN_eDebug("Added good ATSC channel: Frequency %d, Modulation %d",
						parm.frequency, parm.modulation);
					break;
				}
				}
			}
		}
	}
}

void eDVBScan::addChannelToScan(iDVBFrontendParameters *feparm)
{
	/* check if we don't already have that channel ... */
	int type;
	feparm->getSystem(type);

	// Enhanced debug output with actual parameters
	switch(type)
	{
	case iDVBFrontend::feSatellite:
	{
		eDVBFrontendParametersSatellite parm;
		feparm->getDVBS(parm);
		
		// Special handling for very low symbol rates
		if (parm.symbol_rate <= DVB_S_MIN_SYMBOL_RATE)
		{
			// For ultralow symbol rates, adjust parameters to improve likelihood of detection
			if (m_enable_extended_symbolrate && parm.system == eDVBFrontendParametersSatellite::System_DVB_S2)
			{
				// For S2 + ultralow SR, ensure appropriate settings
				parm.fec = eDVBFrontendParametersSatellite::FEC_Auto;
				parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
				parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
				
				// Update parameters with optimized values
				feparm->setDVBS(parm);
				SCAN_eDebug("Optimized ultralow SR transponder (%d): Orbit %d, Freq %d, Pol %d",
					parm.symbol_rate, parm.orbital_position, parm.frequency, parm.polarisation);
			}
		}
		else
		{
			SCAN_eDebug("Try to add satellite transponder: Orbit %d, Freq %d, SR %d, Pol %d, FEC %d, Sys %d, Mod %d",
				parm.orbital_position, parm.frequency, parm.symbol_rate, parm.polarisation, 
				parm.fec, parm.system, parm.modulation);
		}
		break;
	}
	case iDVBFrontend::feCable:
	{
		eDVBFrontendParametersCable parm;
		feparm->getDVBC(parm);
		SCAN_eDebug("Try to add cable transponder: Freq %d, SR %d, Mod %d, FEC %d",
			parm.frequency, parm.symbol_rate, parm.modulation, parm.fec_inner);
		break;
	}
	case iDVBFrontend::feTerrestrial:
	{
		eDVBFrontendParametersTerrestrial parm;
		feparm->getDVBT(parm);
		SCAN_eDebug("Try to add terrestrial transponder: Freq %d, Bandwidth %d, Modulation %d, HP-FEC %d, LP-FEC %d, Transmission %d",
			parm.frequency, parm.bandwidth, parm.modulation, 
			parm.code_rate_HP, parm.code_rate_LP, parm.transmission_mode);
		break;
	}
	case iDVBFrontend::feATSC:
	{
		eDVBFrontendParametersATSC parm;
		feparm->getATSC(parm);
		SCAN_eDebug("Try to add ATSC transponder: Freq %d, Mod %d, Inv %d, Sys %d",
			parm.frequency, parm.modulation, parm.inversion, parm.system);
		break;
	}
	}

	int found_count=0;
	
	// Use a count-based approach to limit duplicate checks
	int check_count = 0;
	const int MAX_CHECKS = 100; // Limit for performance with large scan lists
	
	/* ... in the list of channels to scan */
	for (std::list<ePtr<iDVBFrontendParameters> >::iterator i(m_ch_toScan.begin()); 
		i != m_ch_toScan.end() && check_count < MAX_CHECKS;)
	{
		check_count++;
		if (sameChannel(*i, feparm))
		{
			if (!found_count)
			{
				*i = feparm;  // update
				SCAN_eDebug("Update transponder in scan list");
			}
			else
			{
				SCAN_eDebug("Remove duplicate transponder from scan list");
				m_ch_toScan.erase(i++);
				continue;
			}
			++found_count;
		}
		++i;
	}

	if (found_count > 0)
	{
		SCAN_eDebug("Transponder already in scan todo list");
		return;
	}

	/* ... in the list of successfully scanned channels */
	check_count = 0;
	for (std::list<ePtr<iDVBFrontendParameters> >::const_iterator i(m_ch_scanned.begin()); 
		i != m_ch_scanned.end() && check_count < MAX_CHECKS; ++i, ++check_count)
	{
		if (sameChannel(*i, feparm))
		{
			SCAN_eDebug("Transponder already successfully scanned");
			return;
		}
	}

	/* ... in the list of unavailable channels */
	check_count = 0;
	for (std::list<ePtr<iDVBFrontendParameters> >::const_iterator i(m_ch_unavailable.begin()); 
		i != m_ch_unavailable.end() && check_count < MAX_CHECKS; ++i, ++check_count)
	{
		if (sameChannel(*i, feparm, true))
		{
			SCAN_eDebug("Transponder was scanned but not available");
			
			// For satellite transponders with low SR, we might want to retry anyway
			if (type == iDVBFrontend::feSatellite && m_enable_extended_symbolrate)
			{
				eDVBFrontendParametersSatellite parm;
				feparm->getDVBS(parm);
				
				if (parm.symbol_rate <= 5000000)
				{
					// Don't skip low SR channels marked unavailable - they might need special handling
					SCAN_eDebug("Retry low SR (%d) transponder even though marked unavailable", parm.symbol_rate);
					break; // Exit loop and proceed to add channel
				}
			}
			return;
		}
	}

	/* ... on the current channel */
	if (sameChannel(m_ch_current, feparm))
	{
		SCAN_eDebug("Transponder is current transponder");
		return;
	}

	SCAN_eDebug("Really add transponder to scan list");
	
	// For DVB-S channel with very low symbol rates, add to end of list to prioritize other channels
	if (type == iDVBFrontend::feSatellite)
	{
		eDVBFrontendParametersSatellite parm;
		feparm->getDVBS(parm);
		
		if (parm.symbol_rate <= 2000000)
		{
			// Add low symbol rate transponders to the end of the list
			// This favors standard SR transponders to be scanned first
			m_ch_toScan.push_back(feparm);
			SCAN_eDebug("Added low SR (%d) transponder to end of scan list", parm.symbol_rate);
			return;
		}
	}
	
	/* otherwise, add it to the todo list. */
	m_ch_toScan.push_front(feparm); // better.. then the rotor not turning wild from east to west :)
}

int eDVBScan::sameChannel(iDVBFrontendParameters *ch1, iDVBFrontendParameters *ch2, bool exact) const
{
	if (!ch1 || !ch2)
		return 0;
		
	int type1, type2;
	
	if (ch1->getSystem(type1) || ch2->getSystem(type2))
		return 0;
		
	if (type1 != type2)
		return 0;
		
	switch (type1)
	{
	case iDVBFrontend::feSatellite:
	{
		eDVBFrontendParametersSatellite parm1, parm2;
		if (ch1->getDVBS(parm1) || ch2->getDVBS(parm2))
			return 0;
			
		// Enhanced comparison for satellite channels
		if (parm1.orbital_position != parm2.orbital_position)
			return 0;
			
		// For non-exact, use a reasonable range of values to consider as 'same'
		// This helps reduce duplicate entries and improve scan performance
		if (!exact)
		{
			// For frequency, use a range of +/- 2000 kHz (2MHz)
			if (abs(parm1.frequency - parm2.frequency) > 2000)
				return 0;
				
			// For symbolrate, use a range of +/- 1000 symbols/sec
			if (abs(parm1.symbol_rate - parm2.symbol_rate) > 1000)
				return 0;
				
			// Must be same polarization
			if (parm1.polarisation != parm2.polarisation)
				return 0;
				
			// For multistream, must match stream parameters
			if (parm1.system == parm2.system && 
				parm1.system == eDVBFrontendParametersSatellite::System_DVB_S2)
			{
				// Match multistream parameters
				if (parm1.is_id != parm2.is_id)
					return 0;
					
				if (parm1.pls_code != parm2.pls_code)
					return 0;
					
				if (parm1.pls_mode != parm2.pls_mode)
					return 0;
			}
			
			// If we get here, they're similar enough to consider as the same channel
			return 1;
		}
		else
		{
			// For exact matching, all relevant parameters must match exactly
			if (parm1.frequency != parm2.frequency)
				return 0;
				
			if (parm1.symbol_rate != parm2.symbol_rate)
				return 0;
				
			if (parm1.polarisation != parm2.polarisation)
				return 0;
				
			if (parm1.fec != parm2.fec)
				return 0;
				
			if (parm1.system != parm2.system)
				return 0;
				
			if (parm1.system == eDVBFrontendParametersSatellite::System_DVB_S2)
			{
				if (parm1.modulation != parm2.modulation)
					return 0;
					
				if (parm1.rolloff != parm2.rolloff)
					return 0;
					
				if (parm1.pilot != parm2.pilot)
					return 0;
					
				if (parm1.is_id != parm2.is_id)
					return 0;
					
				if (parm1.pls_code != parm2.pls_code)
					return 0;
					
				if (parm1.pls_mode != parm2.pls_mode)
					return 0;
			}
			
			return 1;
		}
		break;
	}
	case iDVBFrontend::feCable:
	{
		eDVBFrontendParametersCable parm1, parm2;
		if (ch1->getDVBC(parm1) || ch2->getDVBC(parm2))
			return 0;
			
		if (exact)
		{
			if (parm1.frequency != parm2.frequency)
				return 0;
				
			if (parm1.symbol_rate != parm2.symbol_rate)
				return 0;
				
			if (parm1.modulation != parm2.modulation)
				return 0;
				
			if (parm1.fec_inner != parm2.fec_inner)
				return 0;
				
			if (parm1.system != parm2.system)
				return 0;
				
			return 1;
		}
		else
		{
			// For non-exact, use a frequency range of +/- 1000 kHz (1MHz)
			if (abs(parm1.frequency - parm2.frequency) > 1000)
				return 0;
				
			// For non-exact, use a symbolrate range of +/- 100 symbols/sec
			if (abs(parm1.symbol_rate - parm2.symbol_rate) > 100)
				return 0;
				
			return 1;
		}
		break;
	}
	case iDVBFrontend::feTerrestrial:
	{
		eDVBFrontendParametersTerrestrial parm1, parm2;
		if (ch1->getDVBT(parm1) || ch2->getDVBT(parm2))
			return 0;
			
		if (exact)
		{
			if (parm1.frequency != parm2.frequency)
				return 0;
				
			if (parm1.bandwidth != parm2.bandwidth)
				return 0;
				
			if (parm1.code_rate_HP != parm2.code_rate_HP)
				return 0;
				
			if (parm1.code_rate_LP != parm2.code_rate_LP)
				return 0;
				
			if (parm1.modulation != parm2.modulation)
				return 0;
				
			if (parm1.transmission_mode != parm2.transmission_mode)
				return 0;
				
			if (parm1.guard_interval != parm2.guard_interval)
				return 0;
				
			if (parm1.hierarchy != parm2.hierarchy)
				return 0;
				
			if (parm1.system != parm2.system)
				return 0;
				
			if (parm1.plp_id != parm2.plp_id)
				return 0;
				
			return 1;
		}
		else
		{
			// For non-exact, use a frequency range of +/- bandwidth/2
			int allowedDiff = 4000; // Default for 8MHz bandwidth
			if (parm1.bandwidth == eDVBFrontendParametersTerrestrial::Bandwidth_7MHz)
				allowedDiff = 3500;
			else if (parm1.bandwidth == eDVBFrontendParametersTerrestrial::Bandwidth_6MHz)
				allowedDiff = 3000;
				
			if (abs(parm1.frequency - parm2.frequency) > allowedDiff)
				return 0;
				
			// System must match
			if (parm1.system != parm2.system)
				return 0;
				
			// For DVB-T2, PLP ID must match
			if (parm1.system == eDVBFrontendParametersTerrestrial::System_DVB_T2 && 
				parm1.plp_id != parm2.plp_id)
				return 0;
				
			return 1;
		}
		break;
	}
	case iDVBFrontend::feATSC:
	{
		eDVBFrontendParametersATSC parm1, parm2;
		if (ch1->getATSC(parm1) || ch2->getATSC(parm2))
			return 0;
			
		if (exact)
		{
			if (parm1.frequency != parm2.frequency)
				return 0;
				
			if (parm1.modulation != parm2.modulation)
				return 0;
				
			if (parm1.system != parm2.system)
				return 0;
				
			return 1;
		}
		else
		{
			// For non-exact ATSC, use a frequency range of +/- 2000 kHz (2MHz)
			if (abs(parm1.frequency - parm2.frequency) > 2000)
				return 0;
				
			// System must match
			if (parm1.system != parm2.system)
				return 0;
				
			return 1;
		}
		break;
	}
	default:
		return 0;
	}
	return 0;
}

//part 2 end

// part 3 start
void eDVBScan::channelDone()
{
	if (m_ready & validSDT && (!(m_flags & scanOnlyFree) || !m_pmt_running))
	{
		unsigned long hash = 0;

		m_ch_current->getHash(hash);

		eDVBNamespace dvbnamespace = buildNamespace(
			(**m_SDT->getSections().begin()).getOriginalNetworkId(),
			(**m_SDT->getSections().begin()).getTransportStreamId(),
			hash);

		// Process the SDT sections (Service Description Table)
		std::vector<ServiceDescriptionSection*>::const_iterator i;
		for (i = m_SDT->getSections().begin(); i != m_SDT->getSections().end(); ++i)
			processSDT(dvbnamespace, **i);
		m_ready &= ~validSDT;
	}

	if (m_ready & validVCT)
	{
		unsigned long hash = 0;

		m_ch_current->getHash(hash);

		int onid = 0; /* TODO: ATSC ONID? */
		eDVBNamespace dvbnamespace = buildNamespace(
			eOriginalNetworkID(onid),
			(**m_VCT->getSections().begin()).getTransportStreamId(),
			hash);

		SCAN_eDebug("VCT: Processing Virtual Channel Table");
		std::vector<VirtualChannelTableSection*>::const_iterator i;
		for (i = m_VCT->getSections().begin(); i != m_VCT->getSections().end(); ++i)
			processVCT(dvbnamespace, **i, onid);
		m_ready &= ~validVCT;
	}

	if (m_ready & validNIT)
	{
		int system;
		std::list<ePtr<iDVBFrontendParameters> > m_ch_toScan_backup;
		m_ch_current->getSystem(system);
		SCAN_eDebug("Dumping NIT (Network Information Table)");
		if (m_flags & clearToScanOnFirstNIT)
		{
			m_ch_toScan_backup = m_ch_toScan;
			m_ch_toScan.clear();
		}
		std::vector<NetworkInformationSection*>::const_iterator i;
		for (i = m_NIT->getSections().begin(); i != m_NIT->getSections().end(); ++i)
		{
			if (m_networkid && m_networkid != (*i)->getTableIdExtension()) // in NIT this is the network id
			{
				SCAN_eDebug("Ignoring NetworkId %d!", (*i)->getTableIdExtension());
				continue;
			}

			const TransportStreamInfoList &tsinfovec = *(*i)->getTsInfo();

			for (TransportStreamInfoConstIterator tsinfo(tsinfovec.begin());
				tsinfo != tsinfovec.end(); ++tsinfo)
			{
				SCAN_eDebug("TSID: %04x ONID: %04x", (*tsinfo)->getTransportStreamId(),
					(*tsinfo)->getOriginalNetworkId());
				bool T2 = false;
				eDVBFrontendParametersTerrestrial t2transponder;
				eOriginalNetworkID onid = (*tsinfo)->getOriginalNetworkId();
				eTransportStreamID tsid = (*tsinfo)->getTransportStreamId();
				eDVBNamespace ns(0);

				for (DescriptorConstIterator desc = (*tsinfo)->getDescriptors()->begin();
						desc != (*tsinfo)->getDescriptors()->end(); ++desc)
				{
					switch ((*desc)->getTag())
					{
					case CABLE_DELIVERY_SYSTEM_DESCRIPTOR:
					{
						if (system != iDVBFrontend::feCable)
							break; // when current locked transponder is no cable transponder ignore this descriptor
						CableDeliverySystemDescriptor &d = (CableDeliverySystemDescriptor&)**desc;
						ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
						eDVBFrontendParametersCable cable;
						cable.set(d);
						feparm->setDVBC(cable);

						unsigned long hash=0;
						feparm->getHash(hash);
						ns = buildNamespace(onid, tsid, hash);

						addChannelToScan(feparm);
						break;
					}
					case TERRESTRIAL_DELIVERY_SYSTEM_DESCRIPTOR:
					{
						if (system != iDVBFrontend::feTerrestrial)
							break; // when current locked transponder is no terrestrial transponder ignore this descriptor
						TerrestrialDeliverySystemDescriptor &d = (TerrestrialDeliverySystemDescriptor&)**desc;
						ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
						eDVBFrontendParametersTerrestrial terr;
						terr.set(d);
						feparm->setDVBT(terr);
						
						unsigned long hash=0;
						feparm->getHash(hash);
						ns = buildNamespace(onid, tsid, hash);

						addChannelToScan(feparm);
						break;
					}
					case LOGICAL_CHANNEL_DESCRIPTOR:
					{
						// we handle it later
						break;
					}
					case S2_SATELLITE_DELIVERY_SYSTEM_DESCRIPTOR:
					{
						SCAN_eDebug("S2_SATELLITE_DELIVERY_SYSTEM_DESCRIPTOR found");
						if (system != iDVBFrontend::feSatellite)
							break; // when current locked transponder is no satellite transponder ignore this descriptor
						S2SatelliteDeliverySystemDescriptor &d = (S2SatelliteDeliverySystemDescriptor&)**desc;
						ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
						eDVBFrontendParametersSatellite sat;
						sat.set(d);

						// Get current transponder parameters to maintain orbital position, etc.
						eDVBFrontendParametersSatellite p;
						m_ch_current->getDVBS(p);

						// Enhanced S2 Satellite descriptor handling - preserve all multistream parameters
						if (p.is_id != sat.is_id || p.pls_mode != sat.pls_mode || p.pls_code != sat.pls_code)
						{
							// Copy essential parameters from current tuned transponder
							sat.orbital_position = p.orbital_position;
							sat.polarisation = p.polarisation;
							sat.frequency = p.frequency;
							
							// The sat object typically only contains stream ID parameters from the descriptor
							// Make sure we have all essential tuning parameters
							if (sat.symbol_rate == 0)
								sat.symbol_rate = p.symbol_rate;
							
							// Set system to DVB-S2 for multistream
							sat.system = eDVBFrontendParametersSatellite::System_DVB_S2;
							
							// Ensure appropriate modulation and FEC for DVB-S2
							if (sat.modulation == eDVBFrontendParametersSatellite::Modulation_Auto)
								sat.modulation = p.modulation;
							
							if (sat.fec == eDVBFrontendParametersSatellite::FEC_Auto)
								sat.fec = p.fec;
							
							// Special handling for low symbol rates
							if (sat.symbol_rate < 5000000 && m_enable_extended_symbolrate)
							{
								sat.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
								sat.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
							}
							
							// Preserve stream ID parameters from descriptor
							// is_id, pls_mode, and pls_code are already set from the descriptor
							
							feparm->setDVBS(sat);
							SCAN_eDebug("Adding DVB-S2 multistream transponder: freq=%d, symbolrate=%d, is_id=%d, pls_mode=%d, pls_code=%d",
								sat.frequency, sat.symbol_rate, sat.is_id, sat.pls_mode, sat.pls_code);
							addChannelToScan(feparm);
						}
						[[fallthrough]];
					}
					case SATELLITE_DELIVERY_SYSTEM_DESCRIPTOR:
					{
						if (system != iDVBFrontend::feSatellite)
							break; // when current locked transponder is no satellite transponder ignore this descriptor

						SatelliteDeliverySystemDescriptor &d = (SatelliteDeliverySystemDescriptor&)**desc;
						if (d.getFrequency() < 10000)
						{
							SCAN_eDebug("Invalid frequency in satellite descriptor: %d - skipping", d.getFrequency());
							break;
						}

						ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
						eDVBFrontendParametersSatellite sat;
						sat.set(d);
						
						// Enhanced handling for DVB-S/S2 transponders with potential low symbol rates
						if ((*desc)->getTag() == SATELLITE_DELIVERY_SYSTEM_DESCRIPTOR && m_enable_extended_symbolrate)
						{
							// For standard DVB-S descriptor, symbol rates under threshold might need special handling
							if (sat.symbol_rate < 5000000)
							{
								// Get current transponder parameters as reference
								eDVBFrontendParametersSatellite currSat;
								m_ch_current->getDVBS(currSat);
								
								// For very low SRs, we might need to force DVB-S2
								if (sat.symbol_rate <= 1000000)
								{
									// Low SR transponders are typically DVB-S2
									SCAN_eDebug("Very low SR (%d) - setting system to DVB-S2", sat.symbol_rate);
									sat.system = eDVBFrontendParametersSatellite::System_DVB_S2;
									sat.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
									sat.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
								}
								else if (sat.system == eDVBFrontendParametersSatellite::System_DVB_S2)
								{
									// For DVB-S2 transponders with low SR, optimize parameters
									sat.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
									sat.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
								}
							}
						}
						
						feparm->setDVBS(sat);
						addChannelToScan(feparm);
						break;
					}
					case EXTENSION_DESCRIPTOR:
					{
						if (system != iDVBFrontend::feTerrestrial)
							break; // when current locked transponder is no terrestrial transponder ignore this descriptor

						ExtensionDescriptor &d = (ExtensionDescriptor&)**desc;
						switch (d.getExtensionTag())
						{
						case T2_DELIVERY_SYSTEM_DESCRIPTOR:
							T2 = true;
							T2DeliverySystemDescriptor &d = (T2DeliverySystemDescriptor&)**desc;
							t2transponder.set(d);

							// fetch T2 namespace for LCN output, where frequency data may not be in SI table
							ePtr<iDVBFrontend> fe;
							ePtr<iDVBTransponderData> trdata;
							if (!m_channel->getFrontend(fe))
							{
								fe->getTransponderData(trdata, true);
								int freq = trdata->getFrequency();
								long hash = 0xEEEE0000;
								hash |= (freq/1000000)&0xFFFF;
								ns = buildNamespace(onid, tsid, hash);  // used in case LOGICAL_CHANNEL_DESCRIPTOR
							}  // end fetch T2 namespace

							for (T2CellConstIterator cell = d.getCells()->begin();
								cell != d.getCells()->end(); ++cell)
							{
								for (T2FrequencyConstIterator freq = (*cell)->getCentreFrequencies()->begin();
									freq != (*cell)->getCentreFrequencies()->end(); ++freq)
								{
									t2transponder.frequency = (*freq) * 10;
									ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
									feparm->setDVBT(t2transponder);
									addChannelToScan(feparm);
								}
							}
						}
						break;
					}
					case FREQUENCY_LIST_DESCRIPTOR:
					{
						if (system != iDVBFrontend::feTerrestrial)
							break; // when current locked transponder is no terrestrial transponder ignore this descriptor
						if (!T2)
							break;

						FrequencyListDescriptor &d = (FrequencyListDescriptor&)**desc;
						if (d.getCodingType() != 0x03)
							break;

						for (CentreFrequencyConstIterator it = d.getCentreFrequencies()->begin();
								it != d.getCentreFrequencies()->end(); ++it)
						{
							t2transponder.frequency = (*it) * 10;
							ePtr<eDVBFrontendParameters> feparm = new eDVBFrontendParameters;
							feparm->setDVBT(t2transponder);
							addChannelToScan(feparm);
						}
						break;
					}
					default:
						SCAN_eDebug("Descriptor tag: %02x", (*desc)->getTag());
						break;
					}
				}
				// We do this after the main loop because we absolutely need the namespace
				for (DescriptorConstIterator desc = (*tsinfo)->getDescriptors()->begin();
					desc != (*tsinfo)->getDescriptors()->end(); ++desc)
				{
					switch ((*desc)->getTag())
					{
						case LOGICAL_CHANNEL_DESCRIPTOR:
						{
							if (!(system == iDVBFrontend::feTerrestrial || system == iDVBFrontend::feCable))
								break; // when current locked transponder is not terrestrial or cable ignore this descriptor

							if (ns.get() == 0)
								break; // invalid namespace

							int signal = 0;
							ePtr<iDVBFrontend> fe;

							if (!m_channel->getFrontend(fe))
								signal = fe->readFrontendData(iFrontendInformation_ENUMS::signalQuality);

							LogicalChannelDescriptor &d = (LogicalChannelDescriptor&)**desc;
							for (LogicalChannelListConstIterator it = d.getChannelList()->begin(); it != d.getChannelList()->end(); it++)
							{
								LogicalChannel *ch = *it;
								if (ch->getVisibleServiceFlag())
								{
									eDVBDB::getInstance()->addLcnToDB(ns.get(), onid.get(), tsid.get(), eServiceID(ch->getServiceId()).get(), ch->getLogicalChannelNumber(), signal);
									SCAN_eDebug("NAMESPACE: %08x ONID: %04x TSID: %04x SID: %04x LCN: %05d SIGNAL: %08d", 
										ns.get(), onid.get(), tsid.get(), ch->getServiceId(), ch->getLogicalChannelNumber(), signal);
								}
							}
							break;
						}
						default:
							break;
					}
				}
			}

		}

		/* A pitfall is to have the clearToScanOnFirstNIT-flag set, and having channels which have
		   no or invalid NIT. This code will not erase the toScan list unless at least one valid entry
		   has been found.

		   This is not a perfect solution, as the channel could contain a partial NIT. Life's bad.
		*/
		if (m_flags & clearToScanOnFirstNIT)
		{
			if (m_ch_toScan.empty())
			{
				eWarning("clearToScanOnFirstNIT was set, but NIT is invalid. Refusing to stop scan.");
				m_ch_toScan = m_ch_toScan_backup;
			} else
				m_flags &= ~clearToScanOnFirstNIT;
		}
		m_ready &= ~validNIT;
	}

	if (m_pmt_running || (m_ready & m_ready_all) != m_ready_all)
	{
		if (m_abort_current_pmt)
		{
			m_abort_current_pmt = false;
			PMTready(-1);
		}
		return;
	}

	SCAN_eDebug("Transponder search complete!");

	/* If we had services on this channel, we declare
	   this channels as "known good". Add it.

	   (TODO: not yet implemented)
	   a NIT entry could have possible overridden
	   our frontend data with more exact data.

	   (TODO: not yet implemented)
	   the tuning process could have lead to more
	   exact data than the user entered.

	   The channel id was probably corrected
	   by the data written in the SDT. This is
	   important, as "initial transponder lists"
	   usually don't have valid CHIDs (and that's
	   good).

	   These are the reasons for adding the transponder
	   here, and not before.
	*/

	int type;
	if (m_ch_current->getSystem(type))
		type = -1;

	for (m_pmt_in_progress = m_pmts_to_read.begin(); m_pmt_in_progress != m_pmts_to_read.end();)
	{
		eServiceReferenceDVB ref;
		ePtr<eDVBService> service = new eDVBService;

		if (!m_chid_current)
		{
			unsigned long hash = 0;

			m_ch_current->getHash(hash);

			m_chid_current = eDVBChannelID(
				buildNamespace(eOriginalNetworkID(0), m_pat_tsid, hash),
				m_pat_tsid, eOriginalNetworkID(0));
		}

		if (m_pmt_in_progress->second.serviceType == 1)
			SCAN_eDebug("SID %04x is TV", m_pmt_in_progress->first);
		else if (m_pmt_in_progress->second.serviceType == 2)
			SCAN_eDebug("SID %04x is Radio", m_pmt_in_progress->first);
		else
			SCAN_eDebug("SID %04x is Data/Other (ServiceType = %04x)", m_pmt_in_progress->first, m_pmt_in_progress->second.serviceType);

		ref.set(m_chid_current);
		ref.setServiceID(m_pmt_in_progress->first);
		ref.setServiceType(m_pmt_in_progress->second.serviceType);

		if (type != -1)
		{
			char sname[255];
			char pname[255];
			memset(pname, 0, sizeof(pname));
			memset(sname, 0, sizeof(sname));
			switch(type)
			{
				case iDVBFrontend::feSatellite:
				{
					eDVBFrontendParametersSatellite parm;
					m_ch_current->getDVBS(parm);
					snprintf(sname, 255, "%d%c SID 0x%02x",
							parm.frequency/1000,
							parm.polarisation ? 'V' : 'H',
							m_pmt_in_progress->first);
					
					// Enhanced provider info for DVB-S/S2
					snprintf(pname, 255, "%s %s %d%c %d.%d°%c",
						parm.system ? "DVB-S2" : "DVB-S",
						parm.modulation == eDVBFrontendParametersSatellite::Modulation_Auto ? "AUTO" :
						parm.modulation == eDVBFrontendParametersSatellite::Modulation_QPSK ? "QPSK" :
						parm.modulation == eDVBFrontendParametersSatellite::Modulation_8PSK ? "8PSK" :
						parm.modulation == eDVBFrontendParametersSatellite::Modulation_QAM16 ? "QAM16" :
						parm.modulation == eDVBFrontendParametersSatellite::Modulation_16APSK ? "16APSK" : "32APSK",
						parm.frequency/1000,
						parm.polarisation ? 'V' : 'H',
						parm.orbital_position/10,
						parm.orbital_position%10,
						parm.orbital_position > 0 ? 'E' : 'W');
					
					// For multistream transponders, add stream ID info
					if (parm.system == eDVBFrontendParametersSatellite::System_DVB_S2 && 
						parm.is_id != eDVBFrontendParametersSatellite::No_Stream_Id_Filter)
					{
						char mstream[64];
						snprintf(mstream, sizeof(mstream), " IS:%d PLS:%d-%06d", 
							parm.is_id, parm.pls_mode, parm.pls_code);
						strncat(pname, mstream, sizeof(pname) - strlen(pname) - 1);
					}
					
					// For very low symbol rates, add SR info
					if (parm.symbol_rate < 2000000)
					{
						char srinfo[32];
						snprintf(srinfo, sizeof(srinfo), " SR:%d", parm.symbol_rate / 1000);
						strncat(pname, srinfo, sizeof(pname) - strlen(pname) - 1);
					}
					break;
				}
				case iDVBFrontend::feTerrestrial:
				{
					eDVBFrontendParametersTerrestrial parm;
					m_ch_current->getDVBT(parm);
					snprintf(sname, 255, "%d SID 0x%02x",
						parm.frequency/1000,
						m_pmt_in_progress->first);
					
					// Enhanced provider info for DVB-T/T2
					snprintf(pname, 255, "%s %s %dMHz",
						parm.system == eDVBFrontendParametersTerrestrial::System_DVB_T ? "DVB-T" :
						parm.system == eDVBFrontendParametersTerrestrial::System_DVB_T2 ? "DVB-T2" : "DVB-T/T2",
						parm.bandwidth == eDVBFrontendParametersTerrestrial::Bandwidth_8MHz ? "8MHz" :
						parm.bandwidth == eDVBFrontendParametersTerrestrial::Bandwidth_7MHz ? "7MHz" :
						parm.bandwidth == eDVBFrontendParametersTerrestrial::Bandwidth_6MHz ? "6MHz" : "Auto",
						parm.frequency/1000000);
					break;
				}
				case iDVBFrontend::feCable:
				{
					eDVBFrontendParametersCable parm;
					m_ch_current->getDVBC(parm);
					snprintf(sname, 255, "%d SID 0x%02x",
						parm.frequency/1000,
						m_pmt_in_progress->first);
					
					// Enhanced provider info for DVB-C
					snprintf(pname, 255, "DVB-C %s %dMHz SR:%d",
						parm.modulation == eDVBFrontendParametersCable::Modulation_QAM16 ? "QAM16" :
						parm.modulation == eDVBFrontendParametersCable::Modulation_QAM32 ? "QAM32" :
						parm.modulation == eDVBFrontendParametersCable::Modulation_QAM64 ? "QAM64" :
						parm.modulation == eDVBFrontendParametersCable::Modulation_QAM128 ? "QAM128" :
						parm.modulation == eDVBFrontendParametersCable::Modulation_QAM256 ? "QAM256" : "Auto",
						parm.frequency/1000000,
						parm.symbol_rate/1000);
					break;
				}
				case iDVBFrontend::feATSC:
				{
					eDVBFrontendParametersATSC parm;
					m_ch_current->getATSC(parm);
					snprintf(sname, 255, "%d SID 0x%02x",
						parm.frequency/1000,
						m_pmt_in_progress->first);
					
					// Enhanced provider info for ATSC
					snprintf(pname, 255, "ATSC %s %dMHz",
						parm.modulation == eDVBFrontendParametersATSC::Modulation_VSB_8 ? "8VSB" :
						parm.modulation == eDVBFrontendParametersATSC::Modulation_VSB_16 ? "16VSB" :
						parm.modulation == eDVBFrontendParametersATSC::Modulation_QAM_64 ? "QAM64" :
						parm.modulation == eDVBFrontendParametersATSC::Modulation_QAM_256 ? "QAM256" : "Auto",
						parm.frequency/1000000);
					break;
				}
			}
			SCAN_eDebug("Service name: '%s'", sname);
			int tsonid = 0;
			if (m_chid_current)
				tsonid = (m_chid_current.transport_stream_id.get() << 16) | m_chid_current.original_network_id.get();
			service->m_service_name = strip_non_graph(convertDVBUTF8(sname, -1, tsonid, 0));
			service->genSortName();
			service->m_provider_name = strip_non_graph(convertDVBUTF8(pname, -1, tsonid, 0));
		}

		if (!(m_flags & scanOnlyFree) || !m_pmt_in_progress->second.scrambled) {
			m_new_servicerefs.push_back(ref);
			std::pair<std::map<eServiceReferenceDVB, ePtr<eDVBService> >::iterator, bool> i =
				m_new_services.insert(std::pair<eServiceReferenceDVB, ePtr<eDVBService> >(ref, service));
			if (i.second)
			{
				m_last_service = i.first;
				m_event(evtNewService);
			}
		}
		else
			SCAN_eDebug("Don't add encrypted service");
		m_pmts_to_read.erase(m_pmt_in_progress++);
	}

	if (!m_chid_current)
		eWarning("The current channel's ID was not corrected - not adding channel");
	else
	{
		addKnownGoodChannel(m_chid_current, m_ch_current);
		if (m_chid_current)
		{
			switch(type)
			{
				case iDVBFrontend::feSatellite:
				case iDVBFrontend::feTerrestrial:
				case iDVBFrontend::feCable:
				case iDVBFrontend::feATSC:
				{
					ePtr<iDVBFrontend> fe;
					if (!m_channel->getFrontend(fe))
					{
						int frequency = fe->readFrontendData(iFrontendInformation_ENUMS::frequency);
						m_tuner_data.insert(std::pair<eDVBChannelID, int>(m_chid_current, frequency));
					}
				}
				default:
					break;
			}
		}
	}

	m_ch_scanned.push_back(m_ch_current);

	for (std::list<ePtr<iDVBFrontendParameters> >::iterator i(m_ch_toScan.begin()); i != m_ch_toScan.end();)
	{
		if (sameChannel(*i, m_ch_current))
		{
			SCAN_eDebug("Remove duplicate from scan list");
			m_ch_toScan.erase(i++);
			continue;
		}
		++i;
	}

	nextChannel();
}

//part 3 end

//part 4 start
void eDVBScan::start(const eSmartPtrList<iDVBFrontendParameters> &known_transponders, int flags, int networkid)
{
	std::list<ePtr<iDVBFrontendParameters> > *transponderlist = &m_ch_toScan;
	m_flags = flags;
	m_networkid = networkid;
	m_ch_toScan.clear();
	m_ch_scanned.clear();
	m_ch_unavailable.clear();
	m_ch_blindscan.clear();
	m_new_channels.clear();
	m_tuner_data.clear();
	m_new_services.clear();
	m_new_servicerefs.clear();
	m_last_service = m_new_services.end();
	
	// Initialize scan statistics
	m_scan_progress = 0;
	m_scan_progress_total = 0;
	m_scan_state = scanStateInit;

	if (m_flags & scanBlindSearch)
	{
		/*
		 * NOTE: for blindscan, the initial list of transponders does not need to contain valid transponders.
		 * Each of the provided transponders will be iterated (i.e. tuned with blindscan parameter) several times
		 * until a tune failure occurs. Each time a blindscan tune iteration returns ok,
		 * a new transponder has been found and will be scanned.
		 * Each transponder in the initial transponder list causes a full blindscan iteration run.
		 *
		 * Some of the parameters of the initial transponders will be used for the blindscan,
		 * but most will be ignored.
		 *
		 * For DVB-S, you need to provide a list of 4 transponders for each orbital position:
		 * one for each polarity (H/V or L/R), one for each band (hi/lo).
		 * The frequency defines the starting frequency within the desired band.
		 * The symbolrate defines the frequency search range, in MHz (frequency / 1000).
		 * The polarity defines on which polarity the search should run.
		 * All remaining transponder parameters will be ignored.
		 * So for each orbital position, 4 blindscan iteration runs will be done, one for each polarity/band 'quadrant'.
		 *
		 * For DVB-C, only one initial transponder has to be provided.
		 * The frequency defines the start of the blindscan.
		 * The symbolrate defines the frequency search range, in MHz (frequency / 1000000).
		 *
		 * For DVB-T, usually only one initial transponder has to be provided.
		 * The frequency defines the start of the blindscan.
		 * The bandwidth defines both the search step as well as the search bandwidth.
		 */

		SCAN_eDebug("Blind scan requested");
		transponderlist = &m_ch_blindscan;
	}

	if (m_flags & scanRemoveServices)
	{
		eDVBDB::getInstance()->resetLcnDB();
	}

	// Enhanced handling for known transponders
	for (eSmartPtrList<iDVBFrontendParameters>::const_iterator i(known_transponders.begin()); i != known_transponders.end(); ++i)
	{
		bool exist = false;
		for (std::list<ePtr<iDVBFrontendParameters> >::const_iterator ii(transponderlist->begin()); ii != transponderlist->end(); ++ii)
		{
			if (sameChannel(*i, *ii, true))
			{
				exist = true;
				break;
			}
		}
		
		if (!exist)
		{
			// Enhance transponder parameters before adding to scan list
			ePtr<iDVBFrontendParameters> optimized_params = optimizeTransponderParams(*i);
			transponderlist->push_back(optimized_params);
		}
	}
	
	// Set total transponder count for progress reporting
	m_scan_progress_total = transponderlist->size();
	
	// Start scanning
	SCAN_eDebug("Starting scan with %d transponders", m_scan_progress_total);
	m_scan_state = scanStateStart;
	nextChannel();
}


void eDVBScan::insertInto(iDVBChannelList *db, bool backgroundscanresult)
{
	if (m_flags & scanRemoveServices)
	{
		bool clearTerrestrial = false;
		bool clearCable = false;
		std::set<unsigned int> scanned_sat_positions;

		for (std::map<eServiceReferenceDVB, ePtr<eDVBService> >::const_iterator
			service(m_new_services.begin()); service != m_new_services.end(); ++service)
		{
			ePtr<eDVBService> dvb_service;
			if (!db->getService(service->first, dvb_service))
			{
				if (dvb_service->m_flags & eDVBService::dxDontshow)
					service->second->m_flags |= eDVBService::dxDontshow;
			}
		}

		std::list<ePtr<iDVBFrontendParameters> >::iterator it(m_ch_scanned.begin());
		for (;it != m_ch_scanned.end(); ++it)
		{
			if (m_flags & scanDontRemoveUnscanned)
				db->removeServices(&(*(*it)));
			else
			{
				int system;
				(*it)->getSystem(system);
				switch(system)
				{
					case iDVBFrontend::feSatellite:
					{
						eDVBFrontendParametersSatellite sat_parm;
						(*it)->getDVBS(sat_parm);
						scanned_sat_positions.insert(sat_parm.orbital_position);
						break;
					}
					case iDVBFrontend::feTerrestrial:
					{
						clearTerrestrial = true;
						break;
					}
					case iDVBFrontend::feCable:
					{
						clearCable = true;
						break;
					}
					case iDVBFrontend::feATSC:
					{
						eDVBFrontendParametersATSC parm;
						(*it)->getATSC(parm);
						if (parm.system == eDVBFrontendParametersATSC::System_ATSC)
							clearTerrestrial = true;
						else
							clearCable = true;
						break;
					}
				}
			}
		}

		for (it = m_ch_unavailable.begin(); it != m_ch_unavailable.end(); ++it)
		{
			if (m_flags & scanDontRemoveUnscanned)
				db->removeServices(&(*(*it)));
			else
			{
				int system;
				(*it)->getSystem(system);
				switch(system)
				{
					case iDVBFrontend::feSatellite:
					{
						eDVBFrontendParametersSatellite sat_parm;
						(*it)->getDVBS(sat_parm);
						scanned_sat_positions.insert(sat_parm.orbital_position);
						break;
					}
					case iDVBFrontend::feTerrestrial:
					{
						clearTerrestrial = true;
						break;
					}
					case iDVBFrontend::feCable:
					{
						clearCable = true;
						break;
					}
					case iDVBFrontend::feATSC:
					{
						eDVBFrontendParametersATSC parm;
						(*it)->getATSC(parm);
						if (parm.system == eDVBFrontendParametersATSC::System_ATSC)
							clearTerrestrial = true;
						else
							clearCable = true;
						break;
					}
				}
			}
		}

		if (clearTerrestrial)
		{
			eDVBChannelID chid;
			chid.dvbnamespace = 0xEEEE0000;
			db->removeServices(chid);
		}
		if (clearCable)
		{
			eDVBChannelID chid;
			chid.dvbnamespace = 0xFFFF0000;
			db->removeServices(chid);
		}
		for (std::set<unsigned int>::iterator x(scanned_sat_positions.begin()); x != scanned_sat_positions.end(); ++x)
		{
			eDVBChannelID chid;
			if (m_flags & scanDontRemoveFeeds)
				chid.dvbnamespace = eDVBNamespace((*x)<<16);
			db->removeServices(chid, *x);
		}
	}

// part 4 end

// part 5 start

	for (std::map<eDVBChannelID, ePtr<iDVBFrontendParameters> >::const_iterator
			ch(m_new_channels.begin()); ch != m_new_channels.end(); ++ch)
	{
		int system;
		ch->second->getSystem(system);
		std::map<eDVBChannelID, int>::iterator it = m_tuner_data.find(ch->first);

		switch(system)
		{
			case iDVBFrontend::feTerrestrial:
			{
				eDVBFrontendParameters *p = (eDVBFrontendParameters*)&(*ch->second);
				eDVBFrontendParametersTerrestrial parm;
				int freq = it != m_tuner_data.end() ? it->second : 0;
				p->getDVBT(parm);
				
				// Only update frequency if we have valid tuner data
				if (freq > 0)
				{
					SCAN_eDebug("Corrected terrestrial frequency for TSID %04x, ONID %04x, NS %08x: %d (was %d)",
						ch->first.transport_stream_id.get(), ch->first.original_network_id.get(),
						ch->first.dvbnamespace.get(), freq, parm.frequency);
					parm.frequency = freq;
					p->setDVBT(parm);
				}
				break;
			}
			case iDVBFrontend::feSatellite:
			{
				// For satellite, we might want to enhance parameters based on what we learned
				eDVBFrontendParameters *p = (eDVBFrontendParameters*)&(*ch->second);
				eDVBFrontendParametersSatellite parm;
				p->getDVBS(parm);
				
				// If we have more accurate frequency data from the tuner, update it
				if (it != m_tuner_data.end())
				{
					int freq = it->second;
					if (freq > 0 && abs(freq - parm.frequency) < 2000)
					{
						SCAN_eDebug("Corrected satellite frequency for TSID %04x, ONID %04x, NS %08x: %d (was %d)",
							ch->first.transport_stream_id.get(), ch->first.original_network_id.get(),
							ch->first.dvbnamespace.get(), freq, parm.frequency);
						parm.frequency = freq;
						p->setDVBS(parm);
					}
				}
				
				// For low symbol rate transponders, make sure appropriate parameters are set
				if (m_enable_extended_symbolrate && parm.symbol_rate <= 5000000)
				{
					bool modified = false;
					
					// For very low symbol rates, ensure DVB-S2 with appropriate parameters
					if (parm.symbol_rate <= 1000000)
					{
						// If not already S2, set it for very low SR transponders
						if (parm.system != eDVBFrontendParametersSatellite::System_DVB_S2)
						{
							parm.system = eDVBFrontendParametersSatellite::System_DVB_S2;
							modified = true;
						}
						
						// Always use auto settings for rolloff and pilot for best reception
						if (parm.rolloff != eDVBFrontendParametersSatellite::RollOff_auto)
						{
							parm.rolloff = eDVBFrontendParametersSatellite::RollOff_auto;
							modified = true;
						}
						
						if (parm.pilot != eDVBFrontendParametersSatellite::Pilot_Auto)
						{
							parm.pilot = eDVBFrontendParametersSatellite::Pilot_Auto;
							modified = true;
						}
					}
					// For regular low SR transponders (1-5 MHz), ensure auto FEC at minimum
					else if (parm.fec == eDVBFrontendParametersSatellite::FEC_None)
					{
						parm.fec = eDVBFrontendParametersSatellite::FEC_Auto;
						modified = true;
					}
					
					if (modified)
					{
						SCAN_eDebug("Enhanced parameters for low SR transponder (%d): orbit %d, freq %d, pol %d", 
							parm.symbol_rate, parm.orbital_position, parm.frequency, parm.polarisation);
						p->setDVBS(parm);
					}
				}
				break;
			}
			case iDVBFrontend::feCable:
			case iDVBFrontend::feATSC:
				// No specific parameter adjustments for Cable/ATSC currently
				break;
		}

		if (m_flags & scanOnlyFree)
		{
			eDVBFrontendParameters *ptr = (eDVBFrontendParameters*)&(*ch->second);
			ptr->setFlags(iDVBFrontendParameters::flagOnlyFree);
		}

		db->addChannelToList(ch->first, ch->second);
	}

	for (std::map<eServiceReferenceDVB, ePtr<eDVBService> >::const_iterator
		service(m_new_services.begin()); service != m_new_services.end(); ++service)
	{
		ePtr<eDVBService> dvb_service;
		if (!db->getService(service->first, dvb_service))
		{
			if (dvb_service->m_flags & eDVBService::dxNoSDT)
				continue;
			if (!(dvb_service->m_flags & eDVBService::dxHoldName))
			{
				dvb_service->m_service_name = service->second->m_service_name;
				dvb_service->m_service_name_sort = service->second->m_service_name_sort;
			}
			dvb_service->m_provider_name = service->second->m_provider_name;
			if (service->second->m_ca.size())
				dvb_service->m_ca = service->second->m_ca;
			if (!backgroundscanresult) // do not remove new found flags when this is the result of a 'background scan'
				dvb_service->m_flags &= ~eDVBService::dxNewFound;
		}
		else
		{
			db->addService(service->first, service->second);
			if (!(m_flags & scanRemoveServices))
				service->second->m_flags |= eDVBService::dxNewFound;
		}
	}

	if (!backgroundscanresult)
	{
		/* only create a 'Last Scanned' bouquet when this is not the result of a background scan */
		std::string bouquetname = "userbouquet.LastScanned.tv";
		std::string bouquetquery = "FROM BOUQUET \"" + bouquetname + "\" ORDER BY bouquet";
		eServiceReference bouquetref(eServiceReference::idDVB, eServiceReference::flagDirectory, bouquetquery);
		bouquetref.setData(0, 1); /* set bouquet 'servicetype' to tv (even though we probably have both tv and radio channels) */
		eBouquet *bouquet = NULL;
		eServiceReference rootref(eServiceReference::idDVB, eServiceReference::flagDirectory, "FROM BOUQUET \"bouquets.tv\" ORDER BY bouquet");
		
		if (!db->getBouquet(bouquetref, bouquet) && bouquet)
		{
			/* bouquet already exists, empty it before we continue */
			bouquet->m_services.clear();
		}
		else
		{
			/* bouquet doesn't yet exist, create a new one */
			if (!db->getBouquet(rootref, bouquet) && bouquet)
			{
				bouquet->m_services.push_back(bouquetref);
				bouquet->flushChanges();
			}
			/* loading the bouquet seems to be the only way to add it to the bouquet list */
			eDVBDB *dvbdb = eDVBDB::getInstance();
			if (dvbdb) dvbdb->loadBouquet(bouquetname.c_str());
			/* and now that it has been added to the list, we can find it */
			db->getBouquet(bouquetref, bouquet);
		}
		
		if (bouquet)
		{
			bouquet->m_bouquet_name = "Last Scanned";

			// Sort services for better presentation
			std::vector<eServiceReferenceDVB> sorted_services;
			
			// First, group by system type
			std::map<int, std::vector<eServiceReferenceDVB>> services_by_type;
			
			for (std::vector<eServiceReferenceDVB>::const_iterator
				service(m_new_servicerefs.begin()); service != m_new_servicerefs.end(); ++service)
			{
				eServiceReferenceDVB ref = *service;
				int service_type = ref.getServiceType();
				
				// Key by service type - TV, Radio, Data
				services_by_type[service_type].push_back(ref);
			}
			
			// Add TV services first (type 1), then Radio (type 2), then others
			for (int type : {1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20})
			{
				if (services_by_type.find(type) != services_by_type.end())
				{
					std::vector<eServiceReferenceDVB> &type_services = services_by_type[type];
					sorted_services.insert(sorted_services.end(), type_services.begin(), type_services.end());
				}
			}
			
			// If we have sorted services, use them; otherwise use original order
			if (!sorted_services.empty())
			{
				for (std::vector<eServiceReferenceDVB>::const_iterator service(sorted_services.begin()); 
					service != sorted_services.end(); ++service)
				{
					bouquet->m_services.push_back(*service);
				}
			}
			else
			{
				for (std::vector<eServiceReferenceDVB>::const_iterator service(m_new_servicerefs.begin()); 
					service != m_new_servicerefs.end(); ++service)
				{
					bouquet->m_services.push_back(*service);
				}
			}
			
			bouquet->flushChanges();
			eDVBDB::getInstance()->renumberBouquet();
			SCAN_eDebug("'Last Scanned' bouquet created with %zd services", bouquet->m_services.size());
		}
		else
		{
			eDebug("Failed to create 'Last Scanned' bouquet!");
		}
	}
}

RESULT eDVBScan::processSDT(eDVBNamespace dvbnamespace, const ServiceDescriptionSection &sdt)
{
	const ServiceDescriptionList &services = *sdt.getDescriptions();
	SCAN_eDebug("Transport Stream ID (TSID): %04x", sdt.getTransportStreamId());
	eDVBChannelID chid(dvbnamespace, sdt.getTransportStreamId(), sdt.getOriginalNetworkId());

	/* save correct CHID for this channel */
	m_chid_current = chid;

	for (ServiceDescriptionConstIterator s(services.begin()); s != services.end(); ++s)
	{
		unsigned short service_id = (*s)->getServiceId();
		SCAN_eDebugNoNewLineStart("SID %04x: ", service_id);
		bool is_crypted = false;

		std::map<unsigned short, service>::iterator it = m_pmts_to_read.find(service_id);
		if (it != m_pmts_to_read.end())
		{
			if (it->second.scrambled)
			{
				SCAN_eDebugNoNewLine("Scrambled! ");
				is_crypted = true;
			}
			else
				SCAN_eDebugNoNewLine("FTA ");
		}
		SCAN_eDebugNoNewLine("\n");

		if (!(m_flags & scanOnlyFree) || !is_crypted)
		{
			eServiceReferenceDVB ref;
			ePtr<eDVBService> service = new eDVBService;

			ref.set(chid);
			ref.setServiceID(service_id);

			for (DescriptorConstIterator desc = (*s)->getDescriptors()->begin();
					desc != (*s)->getDescriptors()->end(); ++desc)
			{
				switch ((*desc)->getTag())
				{
				case SERVICE_DESCRIPTOR:
				{
					ServiceDescriptor &d = (ServiceDescriptor&)**desc;
					int servicetype = d.getServiceType();
					
					// Enhanced handling for HD services
					if (servicetype == 0x19) // HD Digital TV Service
					{
						// Set the HD flag for this service
						service->m_flags |= eDVBService::dxIsHD;
					}
					else if (servicetype == 0x1c) // Advanced codec HD digital television service
					{
						// Set the HD flag for this service
						service->m_flags |= eDVBService::dxIsHD;
					}
					
					ref.setServiceType(servicetype);
					int tsonid = (sdt.getTransportStreamId() << 16) | sdt.getOriginalNetworkId();
					service->m_service_name = strip_non_graph(convertDVBUTF8(d.getServiceName(), -1, tsonid, 0));
					service->genSortName();

					service->m_provider_name = strip_non_graph(convertDVBUTF8(d.getServiceProviderName(), -1, tsonid, 0));
					SCAN_eDebug("Service name: %s", service->m_service_name.c_str());
					break;
				}
				case CA_IDENTIFIER_DESCRIPTOR:
				{
					CaIdentifierDescriptor &d = (CaIdentifierDescriptor&)**desc;
					const CaSystemIdList &caids = *d.getCaSystemIds();
					for (CaSystemIdList::const_iterator i(caids.begin()); i != caids.end(); ++i)
					{
						SCAN_eDebugNoNewLine(" CA: %04x", *i);
						service->m_ca.push_front(*i);
					}
					SCAN_eDebugNoNewLine("\n");
					break;
				}
				default:
					break;
				}
			}

			if (is_crypted && !service->m_ca.size())
				service->m_ca.push_front(0);

			m_new_servicerefs.push_back(ref);
			std::pair<std::map<eServiceReferenceDVB, ePtr<eDVBService> >::iterator, bool> i =
				m_new_services.insert(std::pair<eServiceReferenceDVB, ePtr<eDVBService> >(ref, service));

			if (i.second)
			{
				m_last_service = i.first;
				m_event(evtNewService);
			}
		}
		if (m_pmt_running && m_pmt_in_progress->first == service_id)
			m_abort_current_pmt = true;
		else
			m_pmts_to_read.erase(service_id);
	}

	return 0;
}

RESULT eDVBScan::processVCT(eDVBNamespace dvbnamespace, const VirtualChannelTableSection &vct, int onid)
{
	const VirtualChannelList &services = *vct.getChannels();
	eDVBChannelID chid(dvbnamespace, vct.getTransportStreamId(), eOriginalNetworkID(onid));

	/* save correct CHID for this channel */
	m_chid_current = chid;

	for (VirtualChannelListConstIterator s(services.begin()); s != services.end(); ++s)
	{
		unsigned short service_id = (*s)->getServiceId();
		unsigned short source_id = (*s)->getSourceId();
		SCAN_eDebugNoNewLineStart("SID %04x, source_id %04x: ", service_id, source_id);
		bool is_crypted = (*s)->isAccessControlled();

		if (is_crypted)
		{
			SCAN_eDebugNoNewLine("is scrambled!");
		}
		else
		{
			SCAN_eDebugNoNewLine("is free");
		}
		SCAN_eDebugNoNewLine("\n");

		if (!(m_flags & scanOnlyFree) || !is_crypted)
		{
			char number[32];
			eServiceReferenceDVB ref;
			ePtr<eDVBService> service = new eDVBService;
			int servicetype = -1;

			if (((*s)->getMajorChannelNumber() & 0x3f0) == 0x3f0)
			{
				snprintf(number, sizeof(number), "%d ", (((*s)->getMajorChannelNumber() & 0x00f) << 10) | (*s)->getMinorChannelNumber());
			}
			else
			{
				snprintf(number, sizeof(number), "%d-%d ", (*s)->getMajorChannelNumber(), (*s)->getMinorChannelNumber());
			}

			switch ((*s)->getServiceType())
			{
			default:
			case 1: /* analog tv */
				break;
			case 2: /* ATSC digital tv */
				servicetype = 1;
				break;
			case 3: /* ATSC audio */
				servicetype = 2;
				break;
			}

			ref.set(chid);
			ref.setServiceID(service_id);
			ref.setServiceType(servicetype);
			ref.setSourceID(source_id);
			service->m_service_name = (*s)->getName();
			/* strip trailing spaces */
			service->m_service_name = service->m_service_name.erase(service->m_service_name.find_last_not_of(" ") + 1);
			/* strip leading spaces */
			service->m_service_name = service->m_service_name.erase(0, service->m_service_name.find_first_not_of(" "));

			for (DescriptorConstIterator desc = (*s)->getDescriptors()->begin();
					desc != (*s)->getDescriptors()->end(); ++desc)
			{
				switch ((*desc)->getTag())
				{
				case 0xa0: /* extended name descriptor */
				{
					ExtendedChannelNameDescriptor &d = (ExtendedChannelNameDescriptor&)**desc;
					if (d.getName().length())
					{
						service->m_service_name = d.getName();
					}
					break;
				}
				default:
					SCAN_eDebug("Descriptor tag: %02x", (*desc)->getTag());
					break;
				}
			}

			service->m_service_name = number + service->m_service_name;

			if (is_crypted && !service->m_ca.size())
				service->m_ca.push_front(0);

			m_new_servicerefs.push_back(ref);
			std::pair<std::map<eServiceReferenceDVB, ePtr<eDVBService> >::iterator, bool> i =
				m_new_services.insert(std::pair<eServiceReferenceDVB, ePtr<eDVBService> >(ref, service));

			if (i.second)
			{
				m_last_service = i.first;
				m_event(evtNewService);
			}
		}
		if (m_pmt_running && m_pmt_in_progress->first == service_id)
			m_abort_current_pmt = true;
		else
			m_pmts_to_read.erase(service_id);
	}

	return 0;
}

RESULT eDVBScan::connectEvent(const sigc::slot<void(int)> &event, ePtr<eConnection> &connection)
{
	connection = new eConnection(this, m_event.connect(event));
	return 0;
}

void eDVBScan::getStats(int &transponders_done, int &transponders_total, int &services)
{
	transponders_done = m_ch_scanned.size() + m_ch_unavailable.size();
	transponders_total = m_ch_toScan.size() + transponders_done;
	services = m_new_services.size();
	
	// Update scan progress
	if (transponders_total > 0)
	{
		m_scan_progress = (transponders_done * 100) / transponders_total;
	}
	else
	{
		m_scan_progress = 0;
	}
}

void eDVBScan::getLastServiceName(std::string &last_service_name)
{
	if (m_last_service == m_new_services.end())
		last_service_name = "";
	else
		last_service_name = m_last_service->second->m_service_name;
}

void eDVBScan::getLastServiceRef(std::string &last_service_ref)
{
	if (m_last_service == m_new_services.end())
		last_service_ref = "";
	else
		last_service_ref = m_last_service->first.toString();
}

RESULT eDVBScan::getFrontend(ePtr<iDVBFrontend> &fe)
{
	if (m_channel)
		return m_channel->getFrontend(fe);
	fe = 0;
	return -1;
}

RESULT eDVBScan::getCurrentTransponder(ePtr<iDVBFrontendParameters> &tp)
{
	if (m_ch_blindscan_result)
	{
		tp = m_ch_blindscan_result;
		return 0;
	}
	else if (m_ch_current)
	{
		tp = m_ch_current;
		return 0;
	}
	tp = 0;
	return -1;
}


// part 5 end
