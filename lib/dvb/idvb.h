#ifndef __dvb_idvb_h
#define __dvb_idvb_h

#ifndef SWIG

#include <linux/dvb/frontend.h>
#include <linux/dvb/video.h>
#include <lib/base/object.h>
#include <lib/base/ebase.h>
#include <lib/base/elock.h>
#include <lib/base/itssource.h>
#include <lib/service/service.h>
#include <libsig_comp.h>
#include <connection.h>
#include <lib/base/nconfig.h> // access to python config
#include <lib/base/estring.h>
// Removed this include to break circular dependency

#define CAID_LIST std::list<uint16_t>

#ifndef DMX_FILTER_SIZE
#define DMX_FILTER_SIZE   16
#endif

struct eDVBSectionFilterMask
{
	int pid;
		/* mode is 0 for positive, 1 for negative filtering */
	uint8_t data[DMX_FILTER_SIZE], mask[DMX_FILTER_SIZE], mode[DMX_FILTER_SIZE];
	enum
	{
		rfCRC=1,
		rfNoAbort=2
	};
	int flags;
};

struct eDVBTableSpec
{
	int pid, tid, tidext, tid_mask, tidext_mask;
	int version;
	int timeout;        /* timeout in ms */
	enum
	{
		tfInOrder		= 1,
		/*
			tfAnyVersion      filter ANY version
			0                 filter all EXCEPT given version (negative filtering)
			tfThisVersion     filter only THIS version
		*/
		tfAnyVersion	= 2,
		tfThisVersion	= 4,
		tfHaveTID		= 8,
		tfHaveTIDExt	= 16,
		tfCheckCRC		= 32,
		tfHaveTimeout	= 64,
		tfHaveTIDMask	= 128,
		tfHaveTIDExtMask= 256
	};
	int flags;
};

struct eBouquet
{
	std::string m_bouquet_name;
	std::string m_filename;  // without path.. just name
	typedef std::list<eServiceReference> list;
	list m_services;
// the following five methods are implemented in db.cpp
	RESULT flushChanges();
	RESULT addService(const eServiceReference &, eServiceReference before=eServiceReference());
	RESULT removeService(const eServiceReference &, bool renameBouquet=true);
	RESULT moveService(const eServiceReference &, unsigned int);
	RESULT setListName(const std::string &name);
};

/* these structures have by intention no operator int() defined.
	the reason of these structures is to avoid mixing for example
	a onid and a tsid (as there's no general order for them).

	defining an operator int() would implicitely convert values
	between them over the constructor with the int argument.

	'explicit' doesn't here - eTransportStreamID(eOriginalNetworkID(n))
	would still work. */

struct eTransportStreamID
{
private:
	int v;
public:
	int get() const { return v; }
	eTransportStreamID(int i): v(i) { }
	eTransportStreamID(): v(-1) { }
	bool operator == (const eTransportStreamID &c) const { return v == c.v; }
	bool operator != (const eTransportStreamID &c) const { return v != c.v; }
	bool operator < (const eTransportStreamID &c) const { return v < c.v; }
	bool operator > (const eTransportStreamID &c) const { return v > c.v; }
};

struct eServiceID
{
private:
	int v;
public:
	int get() const { return v; }
	eServiceID(int i): v(i) { }
	eServiceID(): v(-1) { }
	bool operator == (const eServiceID &c) const { return v == c.v; }
	bool operator != (const eServiceID &c) const { return v != c.v; }
	bool operator < (const eServiceID &c) const { return v < c.v; }
	bool operator > (const eServiceID &c) const { return v > c.v; }
};

struct eOriginalNetworkID
{
private:
	int v;
public:
	int get() const { return v; }
	eOriginalNetworkID(int i): v(i) { }
	eOriginalNetworkID(): v(-1) { }
	bool operator == (const eOriginalNetworkID &c) const { return v == c.v; }
	bool operator != (const eOriginalNetworkID &c) const { return v != c.v; }
	bool operator < (const eOriginalNetworkID &c) const { return v < c.v; }
	bool operator > (const eOriginalNetworkID &c) const { return v > c.v; }
};

struct eDVBNamespace
{
private:
	int v;
public:
	int get() const { return v; }
	eDVBNamespace(int i): v(i) { }
	eDVBNamespace(): v(-1) { }
	bool operator == (const eDVBNamespace &c) const { return v == c.v; }
	bool operator != (const eDVBNamespace &c) const { return v != c.v; }
	bool operator < (const eDVBNamespace &c) const { return v < c.v; }
	bool operator > (const eDVBNamespace &c) const { return v > c.v; }
};

struct eDVBChannelID
{
	eDVBNamespace dvbnamespace;
	eTransportStreamID transport_stream_id;
	eOriginalNetworkID original_network_id;

	bool operator==(const eDVBChannelID &c) const
	{
		return dvbnamespace == c.dvbnamespace &&
			transport_stream_id == c.transport_stream_id &&
			original_network_id == c.original_network_id;
	}

	bool operator<(const eDVBChannelID &c) const
	{
		if (dvbnamespace < c.dvbnamespace)
			return 1;
		else if (dvbnamespace == c.dvbnamespace)
		{
			if (original_network_id < c.original_network_id)
				return 1;
			else if (original_network_id == c.original_network_id)
				if (transport_stream_id < c.transport_stream_id)
					return 1;
		}
		return 0;
	}
	eDVBChannelID(eDVBNamespace dvbnamespace, eTransportStreamID tsid, eOriginalNetworkID onid):
			dvbnamespace(dvbnamespace), transport_stream_id(tsid), original_network_id(onid)
	{
	}
	eDVBChannelID():
			dvbnamespace(-1), transport_stream_id(-1), original_network_id(-1)
	{
	}
	operator bool() const
	{
		return (dvbnamespace != -1) && (transport_stream_id != -1) && (original_network_id != -1);
	}
};

class eServiceReferenceDVB: public eServiceReference
{
public:
	enum service_ref
	{
		ref_service_type= 0,
		ref_service_id	= 1,

	};

	int getServiceType() const { return data[ref_service_type]; }
	void setServiceType(int service_type) { data[ref_service_type]=service_type; }

	eServiceID getServiceID() const { return eServiceID(data[ref_service_id]); }
	void setServiceID(eServiceID service_id) { data[ref_service_id]=service_id.get(); }

	eTransportStreamID getTransportStreamID() const { return eTransportStreamID(data[2]); }
	void setTransportStreamID(eTransportStreamID transport_stream_id) { data[2]=transport_stream_id.get(); }

	eOriginalNetworkID getOriginalNetworkID() const { return eOriginalNetworkID(data[3]); }
	void setOriginalNetworkID(eOriginalNetworkID original_network_id) { data[3]=original_network_id.get(); }

	eDVBNamespace getDVBNamespace() const { return eDVBNamespace(data[4]); }
	void setDVBNamespace(eDVBNamespace dvbnamespace) { data[4]=dvbnamespace.get(); }

	eDVBChannelID getChannelID() const { return eDVBChannelID(getDVBNamespace(), getTransportStreamID(), getOriginalNetworkID()); }
	void setChannelID(const eDVBChannelID &channelid)
	{
		setDVBNamespace(channelid.dvbnamespace);
		setTransportStreamID(channelid.transport_stream_id);
		setOriginalNetworkID(channelid.original_network_id);
	}

	eServiceID getParentServiceID() const { return eServiceID(data[5]); }
	void setParentServiceID( eServiceID sid ) { data[5]=sid.get(); }

	eTransportStreamID getParentTransportStreamID() const { return eTransportStreamID(data[6]); }
	void setParentTransportStreamID( eTransportStreamID tsid ) { data[6]=tsid.get(); }

	eServiceReferenceDVB getParentServiceReference() const
	{
		eServiceReferenceDVB tmp(*this);
		if (data[5] && data[6])
		{
			tmp.data[1] = data[5];
			tmp.data[2] = data[6];
			tmp.data[5] = tmp.data[6] = 0;
		}
		return tmp;
	}

	eServiceReferenceDVB(eDVBNamespace dvbnamespace, eTransportStreamID transport_stream_id, eOriginalNetworkID original_network_id, eServiceID service_id, int service_type)
		:eServiceReference(eServiceReference::idDVB, 0)
	{
		setTransportStreamID(transport_stream_id);
		setOriginalNetworkID(original_network_id);
		setDVBNamespace(dvbnamespace);
		setServiceID(service_id);
		setServiceType(service_type);
	}

	// only for NVOD reference services
	eServiceReferenceDVB(eDVBNamespace dvbnamespace, eTransportStreamID transport_stream_id, eOriginalNetworkID original_network_id, eServiceID service_id, int service_type, eServiceID parent_service_id, eTransportStreamID parent_transport_stream_id)
		:eServiceReference(eServiceReference::idDVB, eServiceReference::flagInvisible)
	{
		setTransportStreamID(transport_stream_id);
		setOriginalNetworkID(original_network_id);
		setDVBNamespace(dvbnamespace);
		setServiceID(service_id);
		setServiceType(service_type);
		setParentServiceID(parent_service_id);
		setParentTransportStreamID(parent_transport_stream_id);
	}

	eServiceReferenceDVB()
		:eServiceReference(eServiceReference::idDVB, 0)
	{
	}

	eServiceReferenceDVB(const std::string &string)
		:eServiceReference(string)
	{
	}

	eServiceReferenceDVB(const eServiceReference &ref)
		:eServiceReference(ref)
	{
	}
};

#endif // SWIG

SWIG_IGNORE(eDVBFrontendParametersSatellite);
class eDVBFrontendParametersSatellite
{
public:
#ifdef SWIG
	eDVBFrontendParametersSatellite();
	~eDVBFrontendParametersSatellite();
#endif
	enum {
		Polarisation_Horizontal, Polarisation_Vertical, Polarisation_CircularLeft, Polarisation_CircularRight
	};

	enum {
		Inversion_Off, Inversion_On, Inversion_Unknown
	};

	enum {
		FEC_Auto=0, FEC_1_2=1, FEC_2_3=2, FEC_3_4=3, FEC_5_6=4, FEC_7_8=5, FEC_8_9=6, FEC_3_5=7, FEC_4_5=8, FEC_9_10=9, FEC_None=15
	};

	enum {
		System_DVB_S, System_DVB_S2
	};

	enum {
		Modulation_Auto, Modulation_QPSK, Modulation_8PSK, Modulation_QAM16
	};

	// dvb-s2
	enum {
		RollOff_alpha_0_35, RollOff_alpha_0_25, RollOff_alpha_0_20, RollOff_auto
	};

	enum {
		Pilot_Off, Pilot_On, Pilot_Unknown
	};

	int no_rotor_command_on_tune;
	unsigned int frequency, symbol_rate;
	int polarisation, fec, inversion, orbital_position, system, modulation, rolloff, pilot;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<eDVBFrontendParametersSatellite>, eDVBFrontendParametersSatellite);

SWIG_IGNORE(eDVBFrontendParametersCable);
class eDVBFrontendParametersCable
{
public:
#ifdef SWIG
	eDVBFrontendParametersCable();
	~eDVBFrontendParametersCable();
#endif
	enum {
		Inversion_Off, Inversion_On, Inversion_Unknown
	};

	/*
	 * WARNING! FEC values and defines, are different from DVB-S(2) implementation!
	 * FEC_Auto and FEC_None are the same as in DVB-S(2), but the values for the other
	 * constants are different!
	 */

	enum {
		FEC_Auto=0, FEC_1_2=1, FEC_2_3=2, FEC_3_4=3, FEC_5_6=4, FEC_7_8=5, FEC_8_9=6, FEC_None=15
	};

	enum {
		System_DVB_C_ANNEX_A, System_DVB_C_ANNEX_C
	};

	enum {
		Modulation_Auto, Modulation_QAM16, Modulation_QAM32, Modulation_QAM64, Modulation_QAM128, Modulation_QAM256
	};

	unsigned int frequency, symbol_rate;
	int modulation, inversion, fec_inner, system;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<eDVBFrontendParametersCable>, eDVBFrontendParametersCable);

SWIG_IGNORE(eDVBFrontendParametersATSC);
class eDVBFrontendParametersATSC
{
public:
#ifdef SWIG
	eDVBFrontendParametersATSC();
	~eDVBFrontendParametersATSC();
#endif
	enum {
		Inversion_Off, Inversion_On, Inversion_Unknown
	};

	enum {
		System_ATSC, System_DVB_C_ANNEX_B
	};

	enum {
		Modulation_Auto, Modulation_QAM16, Modulation_QAM32, Modulation_QAM64, Modulation_QAM128, Modulation_QAM256, Modulation_VSB_8, Modulation_VSB_16
	};

	unsigned int frequency;
	int modulation, inversion, system;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<eDVBFrontendParametersATSC>, eDVBFrontendParametersATSC);

SWIG_IGNORE(eDVBFrontendParametersTerrestrial);
class eDVBFrontendParametersTerrestrial
{
public:
#ifdef SWIG
	eDVBFrontendParametersTerrestrial();
	~eDVBFrontendParametersTerrestrial();
#endif
	enum {
		Bandwidth_8MHz, Bandwidth_7MHz, Bandwidth_6MHz, Bandwidth_Auto, Bandwidth_5MHz, Bandwidth_1_712MHz, Bandwidth_10MHz
	};

	enum {
		FEC_1_2=0, FEC_2_3=1, FEC_3_4=2, FEC_5_6=3, FEC_7_8=4, FEC_Auto=5, FEC_6_7=6, FEC_8_9=7
	};

	enum {
		System_DVB_T_T2, System_DVB_T, System_DVB_T2
	};

	enum {
		TransmissionMode_2k, TransmissionMode_8k, TransmissionMode_Auto, TransmissionMode_4k, TransmissionMode_1k, TransmissionMode_16k, TransmissionMode_32k
	};

	enum {
		GuardInterval_1_32, GuardInterval_1_16, GuardInterval_1_8, GuardInterval_1_4, GuardInterval_Auto, GuardInterval_1_128, GuardInterval_19_128, GuardInterval_19_256
	};

	enum {
		Hierarchy_None, Hierarchy_1, Hierarchy_2, Hierarchy_4, Hierarchy_Auto
	};

	enum {
		Modulation_QPSK, Modulation_QAM16, Modulation_QAM64, Modulation_Auto, Modulation_QAM256
	};

	enum {
		Inversion_Off, Inversion_On, Inversion_Unknown
	};

	unsigned int frequency;
	int bandwidth;
	int code_rate_HP, code_rate_LP;
	int modulation;
	int transmission_mode;
	int guard_interval;
	int hierarchy;
	int inversion;
	int system;
	int plp_id;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<eDVBFrontendParametersTerrestrial>, eDVBFrontendParametersTerrestrial);

class eDVBEvent
{
public:
	time_t m_start_time;
	int m_duration;
	std::string m_event_name;
	std::string m_short_description;
	std::string m_extended_description;
};

class eComponentData
{
public:
	friend class eServiceEvent;
	SWIG_ALLOW_OUTPUT_SIMPLE(eComponentData);
private:
	int m_streamContent;
	int m_componentType;
	int m_componentTag;
	std::string m_iso639LanguageCode;
	std::string m_text;
public:
	eComponentData(int streamContent, int componentType, int componentTag, const std::string &iso639LanguageCode, const std::string &text) :
		m_streamContent(streamContent),
		m_componentType(componentType),
		m_componentTag(componentTag),
		m_iso639LanguageCode(iso639LanguageCode),
		m_text(text)
	{}
	eComponentData():
		m_streamContent(0),
		m_componentType(0),
		m_componentTag(0)
	{}
	int getStreamContent(void) const { return m_streamContent; }
	int getComponentType(void) const { return m_componentType; }
	int getComponentTag(void) const { return m_componentTag; }
	const std::string &getIso639LanguageCode(void) const { return m_iso639LanguageCode; }
	const std::string &getText(void) const { return m_text; }
};

#ifndef SWIG
class eServiceEvent: public iObject
{
	DECLARE_REF(eServiceEvent);
public:
	int m_begin, m_duration;
	int m_event_id;
	std::string m_service_reference;
	std::string m_event_name;
	std::string m_short_description;
	std::string m_extended_description;
	std::vector<eComponentData> m_component_data;
	std::string m_rating;

	eServiceEvent();
#endif
};
SWIG_TEMPLATE_TYPEDEF(ePtr<eServiceEvent>, eServiceEvent);

class iDVBSectionListener
{
public:
	virtual ~iDVBSectionListener() {}
	virtual void handleSection(const uint8_t *data, unsigned int size, unsigned int pid) = 0;
};

/* Implemented Interfaces: */

#ifndef SWIG

class iDVBPVRChannel_ENUMS
{
public:
	enum
	{
		state_idle,		//0
		state_preparing,	//1
		state_tuning,	//2
		state_running,	//3
		state_paused,	//4
		state_seeking,	//5
		state_eof		//6
	};
	enum
	{
		timeshift_enabled=1
	};
};

class iDVBPVRService_ENUMS
{
public:
	enum
	{
		canDescent=1
	};
};

class iDVBFrontend_ENUMS
{
#ifdef SWIG
	iDVBFrontend_ENUMS();
	~iDVBFrontend_ENUMS();
#endif
public:
	enum { feSatellite, feCable, feTerrestrial, feATSC };
	enum { stateIdle, stateTuning, stateFailed, stateLock, stateLostLock, stateClosed };
	enum { toneOff, toneOn };
	enum { voltageOff, voltage13, voltage18, voltage13_5, voltage18_5, voltage5_terrestrial };
	
	// Frontend information enum values
	enum { 
		bitErrorRate, 
		snrValue, 
		signalQuality, 
		signalQualitydB, 
		signalPower, 
		lockState, 
		syncState, 
		frontendNumber, 
		frontendStatus,
		frequency,
		symbolRate,
		isUsbTuner,
		modcodValue  // New enum for MODCOD information
	};
};

class iDVBFrontendStatus:  public iDVBFrontend_ENUMS, public iObject
{
public:
	virtual int getState() const = 0;
	virtual std::string getStateDescription() const = 0;
	virtual int getLocked() const = 0;
	virtual int getSynced() const = 0;
	virtual int getBER() const = 0;
	virtual int getSNR() const = 0;
	virtual int getSNRdB() const = 0;
	virtual int getSignalPower() const = 0;
};

class iDVBTransponderData: public iObject
{
public:
	virtual std::string getTunerType() const = 0;
	virtual int getInversion() const = 0;
	virtual unsigned int getFrequency() const = 0;
	virtual unsigned int getSymbolRate() const = 0;
	virtual int getOrbitalPosition() const = 0;
	virtual int getFecInner() const = 0;
	virtual int getModulation() const = 0;
	virtual int getPolarization() const = 0;
	virtual int getRolloff() const = 0;
	virtual int getPilot() const = 0;
	virtual int getSystem() const = 0;
	virtual int getIsId() const = 0;
	virtual int getPLSMode() const = 0;
	virtual int getPLSCode() const = 0;
	virtual int getT2MIPlpId() const = 0;
	virtual int getT2MIPid() const = 0;
	virtual int getBandwidth() const = 0;
	virtual int getCodeRateLp() const = 0;
	virtual int getCodeRateHp() const = 0;
	virtual int getConstellation() const = 0;
	virtual int getTransmissionMode() const = 0;
	virtual int getGuardInterval() const = 0;
	virtual int getHierarchyInformation() const = 0;
	virtual int getPlpId() const = 0;
	virtual int getMODCOD() const = 0;
	virtual std::string getMODCODDescription() const = 0;
	virtual int getRequiredSNR() const = 0;
};

// Adding MODCOD value to the existing iDVBFrontend_ENUMS
// Do not redefine iFrontendInformation_ENUMS here - it's already defined in iservice.h

#include <lib/service/iservice.h> // Include this to use iFrontendInformation_ENUMS

// Forward declarations for classes defined in dvb/sec.h
class eDVBDiseqcCommand;
class eSecCommandList;
class iDVBSatelliteEquipmentControl;

// Use iFrontendInformation from iservice.h instead of redefining it
/* class iFrontendInformation: public iObject, public iFrontendInformation_ENUMS
{
public:
	virtual int getFrontendInfo(int w)=0;
	virtual SWIG_VOID(RESULT) getLockStatus(int &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) getFrontendStatus(int &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) getTransponderData(struct eDVBTransponderData &SWIG_OUTPUT, bool SWIG_DEFAULT(original))=0;
		/* read only the frontend tuner data, without touching the current state */
	virtual SWIG_VOID(RESULT) getFrontendData(struct eDVBFrontendParametersSatellite &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) getFrontendData(struct eDVBFrontendParametersCable &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) getFrontendData(struct eDVBFrontendParametersTerrestrial &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) getFrontendData(struct eDVBFrontendParametersATSC &SWIG_OUTPUT)=0;
};*/

class iDVBFrontend: public iObject, public iDVBFrontend_ENUMS
{
public:
	virtual RESULT tune(const iDVBFrontendParameters &where)=0;
	virtual int getCapabilities()=0;
	virtual RESULT getState()=0;
	virtual RESULT setTone(int tone)=0;
	virtual RESULT setVoltage(int voltage)=0;
	virtual RESULT sendDiseqc(const eDVBDiseqcCommand &diseqc)=0;
	virtual RESULT sendToneburst(int burst)=0;
#ifndef SWIG
	virtual RESULT setDeliverySystem(const char *type)=0;
#endif
	virtual RESULT setSEC(iDVBSatelliteEquipmentControl *sec)=0;
	virtual RESULT setSecSequence(eSecCommandList &list)=0;
	virtual RESULT getData(int num, long &data)=0;
	virtual RESULT setData(int num, long val)=0;
		/* 0 means: not compatible. other values are a priority. highest wins. */
	virtual int isCompatibleWith(ePtr<iDVBFrontendParameters> &feparm)=0;
	virtual int readFrontendData(int type)=0;
	virtual void getFrontendStatus(ePtr<iDVBFrontendStatus> &dest)=0;
	virtual void getTransponderData(ePtr<iDVBTransponderData> &dest, bool original)=0;
	virtual void getFrontendData(ePtr<iDVBFrontendData> &dest)=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBFrontend>, iDVBFrontendPtr);

// MODCOD constants and SNR requirements for DVB-S2
namespace DVB_S2_MODCOD {
    enum {
        DUMMY = 0,
        QPSK_1_4 = 1,
        QPSK_1_3 = 2,
        QPSK_2_5 = 3,
        QPSK_1_2 = 4,
        QPSK_3_5 = 5,
        QPSK_2_3 = 6,
        QPSK_3_4 = 7,
        QPSK_4_5 = 8,
        QPSK_5_6 = 9,
        QPSK_8_9 = 10,
        QPSK_9_10 = 11,
        PSK8_3_5 = 12,
        PSK8_2_3 = 13,
        PSK8_3_4 = 14,
        PSK8_5_6 = 15,
        PSK8_8_9 = 16,
        PSK8_9_10 = 17,
        APSK16_2_3 = 18,
        APSK16_3_4 = 19,
        APSK16_4_5 = 20,
        APSK16_5_6 = 21,
        APSK16_8_9 = 22,
        APSK16_9_10 = 23,
        APSK32_3_4 = 24,
        APSK32_4_5 = 25,
        APSK32_5_6 = 26,
        APSK32_8_9 = 27,
        APSK32_9_10 = 28
    };
    
    // Helper array with required SNR values in dB * 10
    static const int requiredSNR_x10[] = {
        0,    // DUMMY
        10,   // QPSK_1_4: 1.0 dB
        31,   // QPSK_1_3: 3.1 dB
        33,   // QPSK_2_5: 3.3 dB
        41,   // QPSK_1_2: 4.1 dB
        48,   // QPSK_3_5: 4.8 dB
        52,   // QPSK_2_3: 5.2 dB
        60,   // QPSK_3_4: 6.0 dB
        64,   // QPSK_4_5: 6.4 dB
        67,   // QPSK_5_6: 6.7 dB
        74,   // QPSK_8_9: 7.4 dB
        75,   // QPSK_9_10: 7.5 dB
        78,   // 8PSK_3_5: 7.8 dB
        83,   // 8PSK_2_3: 8.3 dB
        94,   // 8PSK_3_4: 9.4 dB
        107,  // 8PSK_5_6: 10.7 dB
        118,  // 8PSK_8_9: 11.8 dB
        120,  // 8PSK_9_10: 12.0 dB
        110,  // 16APSK_2_3: 11.0 dB
        123,  // 16APSK_3_4: 12.3 dB
        131,  // 16APSK_4_5: 13.1 dB
        135,  // 16APSK_5_6: 13.5 dB
        145,  // 16APSK_8_9: 14.5 dB
        148,  // 16APSK_9_10: 14.8 dB
        161,  // 32APSK_3_4: 16.1 dB
        171,  // 32APSK_4_5: 17.1 dB
        176,  // 32APSK_5_6: 17.6 dB
        194,  // 32APSK_8_9: 19.4 dB
        198   // 32APSK_9_10: 19.8 dB
    };
};

class iDVBChannel_ENUMS
{
#ifdef SWIG
	iDVBChannel_ENUMS();
	~iDVBChannel_ENUMS();
#endif
public:
	enum 
	{
		state_idle,
		state_tuning,
		state_failed,
		state_unavailable,
		state_ok,
		state_last_instance
	};
	enum
	{
		flagPreferredLock=1
	};
};

class iDVBChannel: public iObject, public iDVBChannel_ENUMS
{
public:
	virtual SWIG_VOID(RESULT) getFrontend(ePtr<iDVBFrontend> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) getCurrentFrontendParameters(ePtr<iDVBFrontendParameters> &SWIG_OUTPUT)=0;
	virtual RESULT requestTsidOnid() = 0;
	virtual int reserveDemux() = 0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBChannel>, iDVBChannelPtr);

class iDVBChannelList: public iObject
{
public:
	virtual SWIG_VOID(RESULT) getChannelByChannelID(const eDVBChannelID &chid, ePtr<iDVBChannel> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) initChannel(ePtr<iDVBChannel> &SWIG_OUTPUT, const eDVBChannelID &chid=eDVBChannelID())=0;
	virtual SWIG_VOID(RESULT) removeChannel(const eDVBChannelID &chid)=0;
	virtual SWIG_VOID(RESULT) flushChannels()=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBChannelList>, iDVBChannelListPtr);

class iDVBSectionReader: public iObject
{
public:
	virtual RESULT setBufferSize(int size)=0;
	virtual RESULT connectRead(const sigc::slot<void(const uint8_t*, int)> &read)=0;
	virtual RESULT start(const eDVBSectionFilterMask &mask)=0;
	virtual RESULT stop()=0;
	virtual ~iDVBSectionReader() {}
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBSectionReader>, iDVBSectionReaderPtr);

class iDVBPESReader: public iObject
{
public:
	virtual SWIG_VOID(RESULT) setBufferSize(int size)=0;
	virtual RESULT connectRead(const sigc::slot<void(const uint8_t*, int)> &read)=0;
	virtual RESULT start(int pid)=0;
	virtual RESULT stop()=0;
	virtual ~iDVBPESReader() {}
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBPESReader>, iDVBPESReaderPtr);

	/* records a given set of pids into a file. not for the faint hearted. */
class iDVBTSRecorder: public iObject
{
public:
	virtual RESULT setBufferSize(int size) = 0;
	virtual RESULT start() = 0;
	virtual RESULT addPID(int pid) = 0;
	virtual RESULT removePID(int pid) = 0;

	enum timing_pid_type { none = 0, video_pid, audio_pid };
	virtual RESULT setTimingPID(int pid, timing_pid_type pidtype, int streamtype) = 0;

	virtual RESULT setTargetFD(int fd) = 0;
	virtual RESULT setBoundary(off_t max) = 0;
	virtual RESULT enableAccessPoints(bool enable) = 0;
	virtual RESULT stop() = 0;

	virtual RESULT getCurrentPCR(pts_t &pcr) = 0;
	virtual RESULT getFirstPTS(pts_t &pts) = 0;

	enum {
		eventWriteError,
				/* a write error has occured. data won't get lost if fd is writable after return. */
		eventReachedBoundary,
				/* the programmed boundary was reached. eof occurred. */
	};
	virtual SWIG_VOID(RESULT) connectEvent(const sigc::slot<void(int)> &event, ePtr<eConnection> &conn)=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBTSRecorder>, iDVBTSRecorderPtr);

class iTSMPEGDecoder: public iObject
{
public:
	enum { pidTypeVideo = 0, pidTypeAudio, pidTypeTeletext, pidTypeSubtitle, pidTypePcr };

	virtual RESULT setSource(ePtr<iTsSource> &source, const char *streaminfo_file) = 0;
	
	virtual RESULT set(int type, int pid) = 0;

	virtual int getPesPid(int type) = 0;

	virtual RESULT set(int type, const int &entry) = 0;

	virtual RESULT setSyncPCR(int pcrpid) = 0;
	virtual RESULT setTextPID(int textpid) = 0;
	virtual RESULT setSyncTS(int pcrpid) = 0;

	enum {
		eventSeek,		/* discont in video stream, i.e. after skip. decoder will pause */
		eventMisconfiguration, 	/* hardware decoder misconfigured */
		eventNoVideo,		/* no video for >= 500ms - may indicate no or broken video stream */
		eventStillPicture,	/* still picture */
		eventPreStart,		/* before playback of video really starts, arrived when video device opened */
		eventHBBTVBoot,		/* issued when a HbbTV app requires the browser to be started */
		eventHBBTVClose,	/* issued when a HbbTV app requires the browser to be stopped */
		eventHBBTVUrl		/* issued when a HbbTV apps wants to load a new url */
	};
	virtual SWIG_VOID(RESULT) connectVideoEvent(const sigc::slot<void(int)> &event, ePtr<eConnection> &SWIG_OUTPUT)=0;

	virtual RESULT connectStateEvent(const sigc::slot<void(int)> &event, ePtr<eConnection> &connection) = 0;

	virtual RESULT start(int pid, int pidtype, int pidtable = 0) = 0;
	virtual RESULT stop() = 0;
	virtual RESULT flush() = 0;
	virtual RESULT showSinglePic(const char *filename) = 0;
	virtual RESULT setFastForward(int skip=0) = 0;	
	virtual RESULT setSlowMotion(int repeat) = 0;
	virtual RESULT setZoom(int zoom) = 0;
	virtual RESULT setFreezeMode(int hold) = 0;
	virtual SWIG_VOID(RESULT) getPTS(pts_t &SWIG_OUTPUT) = 0;
	virtual RESULT lockNewPTSvalues() = 0;
	virtual RESULT unlockNewPTSvalues() = 0;

	virtual RESULT pause() = 0;
	virtual RESULT unpause() = 0;
	virtual RESULT setFastForward_internal(int skip=0) = 0;

	virtual RESULT getVideoInfo(int &width, int &height, int &aspect, int &fps) = 0;
	virtual RESULT getVideoProgressive(int &progressive) = 0;
	virtual RESULT getVideoFrameRate(int &framerate) = 0;

	virtual int getAspect(void) = 0;
	virtual int getVideoWidth(void) = 0;
	virtual int getVideoHeight(void) = 0;
	virtual int getVideoProgressiveFrameCount(void) = 0;
	virtual int getVideoProgressiveCount(void) = 0;
	virtual int getVideoFrameRate(void) = 0;

	virtual RESULT setRadioPic(const std::string &filename) = 0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iTSMPEGDecoder>, iTSMPEGDecoderPtr);

class iStreamData;
SWIG_TEMPLATE_DECLARE(ePtr<iStreamData>, iStreamDataPtr);

class iStreamBufferInfo: public iObject
{
public:
	virtual int getSize()=0;
	virtual int getAvailable()=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iStreamBufferInfo>, iStreamBufferInfoPtr);

class iStreamData: public iObject
{
public:
	enum StreamDataType
	{
		TypePES,
		TypeTS,
		TypeAudioMp3,
		TypeAudioMp2,
		TypeAudioMpega,
		TypeAudioAc3,
		TypeAudioDts,
		TypeAudioAac,
		TypeAudioLpcm,
		TypeAudioWav,
		TypeVideoH264,
		TypeVideoH265,
		TypeVideoH265_DRM,
		TypeVideoMpeg2,
		TypeVideoMpeg4,
		TypeVideoVc1,
		TypeVideoVp8,
		TypeVideoVp9,
		TypeAvMpeg,
		TypeAvMpeg4,
		TypeAvMkv,
		TypeAvWebm,
		TypeAvAvi,
		TypeAvFLV,
	};

	virtual SWIG_VOID(RESULT) getStreamDataBuffer(ePtr<iStreamBufferInfo> &SWIG_OUTPUT) = 0;
	virtual RESULT read(off_t offset, void *buf, size_t count) = 0;
	virtual off_t length() = 0;
	virtual off_t offset() = 0;
	virtual RESULT getEncryptionInfo(ssize_t &, ssize_t &, ssize_t &) = 0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iStreamData>, iStreamDataPtr);

class iServiceHandler: public iObject
{
public:
	virtual SWIG_VOID(RESULT) play(const eServiceReference &service, ePtr<iPlayableService> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) record(const eServiceReference &service, ePtr<iRecordableService> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) list(const eServiceReference &service, ePtr<iListableService> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) info(const eServiceReference &service, ePtr<iStaticServiceInformation> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) offlineOperations(const eServiceReference &service, ePtr<iServiceOfflineOperations> &SWIG_OUTPUT)=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iServiceHandler>, iServiceHandlerPtr);

class iDVBServiceList;
SWIG_TEMPLATE_DECLARE(ePtr<iDVBServiceList>, iDVBServiceListPtr);

class iListable: public iObject
{
public:
	virtual RESULT getContent(std::list<eServiceReference> &list, bool sorted=false)=0;
};

class iDVBServiceList: public iListable
{
public:
	virtual ~iDVBServiceList() {}
	virtual SWIG_VOID(RESULT) startEdit(ePtr<eDVBServiceList> &SWIG_OUTPUT)=0;
	virtual SWIG_VOID(RESULT) flushChanges()=0;
	virtual SWIG_VOID(RESULT) addService(const eServiceReference &reference, eDVBService *service)=0;
	virtual SWIG_VOID(RESULT) removeService(const eServiceReference &reference, bool renameBouquet=true)=0;
	virtual SWIG_VOID(RESULT) updateService(const eServiceReference &reference, eDVBService *service)=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBServiceList>, iDVBServiceListPtr);

#define declareI(x) \
public: \
	virtual SWIG_VOID(RESULT) get##x(ePtr<iDVB##x> &SWIG_OUTPUT)=0;
#define declareE(x) \
public: \
	virtual ePtr<iDVB##x> get##x()=0;

class iDVBManager: public iObject
{
	declareI(ResourceManager);
	declareI(ChannelList);
	declareI(FrontendList);
	declareI(Demux);
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBManager>, iDVBManagerPtr);

class iDVBResourceManager: public iObject
{
public:
	enum {
		errorNoFrontend = -1,
		errorNoFrontend_available = -2,
		errorNoChannelList = -3,
	};
	virtual SWIG_VOID(RESULT) allocateRawChannel(const eDVBChannelID &chid, eUsePtr<iDVBChannel> &SWIG_OUTPUT, int simulate=0)=0;
	virtual RESULT setFrontendSlotInformations(std::vector<eDVBFrontendParametersSatellite> &, std::vector<eDVBFrontendParametersTerrestrial> &, std::vector<eDVBFrontendParametersCable> &, std::vector<eDVBFrontendParametersATSC> &, int sort=0)=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBResourceManager>, iDVBResourceManagerPtr);

class iDVBSatelliteEquipmentControl;
SWIG_TEMPLATE_DECLARE(ePtr<iDVBSatelliteEquipmentControl>, iDVBSatelliteEquipmentControlPtr);

class iDVBSatelliteEquipmentControl: public iObject
{
public:
	/* DEPRECATED: use setCable */
	virtual RESULT prepare(iDVBFrontend &frontend, const eDVBFrontendParametersSatellite &sat, int &frequency, int frontend_id, unsigned int timeout)=0;
	virtual RESULT prepareSTeletext(iDVBFrontend &frontend, const eDVBFrontendParametersSatellite &sat, int &frequency, int frontend_id, unsigned int timeout)=0;
	
	virtual RESULT prepareFrontend(iDVBFrontend &frontend, const eDVBFrontendParametersSatellite &sat, int frontend_id)=0;

	virtual int canTune(const eDVBFrontendParametersSatellite &feparm, iDVBFrontend *fe, int frontend_id, int *highest_score_lnb=0)=0;
	virtual void setRotorMoving(int slot_id, bool)=0;
};
SWIG_TEMPLATE_TYPEDEF(ePtr<iDVBSatelliteEquipmentControl>, iDVBSatelliteEquipmentControlPtr);

#endif

#endif