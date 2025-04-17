/*
 * Improved DVB Reader - A more robust DVB transport stream parser
 * 
 * This is an enhanced version of the dvbreader module used for scanning DVB
 * transport streams. It maintains the same API but improves reliability,
 * provides better timeouts, adds retry mechanisms, and enhances error handling.
 */

#include <Python.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/dmx.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

// Define UNUSED macro to address unused parameter warnings
#define UNUSED(x) (void)(x)

// Define constants
#define TS_PACKET_SIZE 188
#define MAX_SECTION_SIZE 4096
#define DEFAULT_SECTION_TIMEOUT_MS 1000    // Adjusted value for better responsiveness
#define DEFAULT_COMPLETE_TIMEOUT_MS 30000  // Adjusted timeout
#define MAX_RETRY_COUNT 8                  // Reduced retries for better UI responsiveness
#define SECTION_HEADER_LENGTH 3
// Debug macro - change to 1 to enable debug output
#define DEBUG_DVBREADER 1

#if DEBUG_DVBREADER
#define DEBUG_PRINT(fmt, args...) fprintf(stderr, "[DVBReader] " fmt, ## args)
#else
#define DEBUG_PRINT(fmt, args...)
#endif

// Structure to hold filter context
typedef struct {
	int fd;                     // File descriptor for the demux device
	int feid;                   // Frontend ID
	unsigned char filter_value; // Filter value (table_id)
	unsigned char filter_mask;  // Filter mask
	unsigned short pid;         // PID to filter
	unsigned char buffer[MAX_SECTION_SIZE]; // Buffer for section data
	int buffer_used;            // Current buffer usage
	int timeout_ms;             // Timeout for reading a section in milliseconds
	int complete_timeout_ms;    // Timeout for completing all sections in milliseconds
	int retry_count;            // Number of retries for failed reads
	time_t start_time;          // Start time for operation
} dvb_filter_context;

// Global settings - can be adjusted by Python code
static int g_section_timeout_ms = DEFAULT_SECTION_TIMEOUT_MS;
static int g_complete_timeout_ms = DEFAULT_COMPLETE_TIMEOUT_MS;
static int g_retry_count = MAX_RETRY_COUNT;

// Helper function to set up the demux filter
static int setup_filter(int fd, uint16_t pid, uint8_t table_id, uint8_t mask, int feid) {
	struct dmx_sct_filter_params filter;
	
	UNUSED(feid); // Mark as unused to avoid warning
	
	memset(&filter, 0, sizeof(filter));
	
	filter.pid = pid;
	
	// Set up filter for the requested table ID
	filter.filter.filter[0] = table_id;
	filter.filter.mask[0] = mask;
	
	// Set up filter for section start
	filter.filter.filter[1] = 0x00; // Table ID extension (not filtered)
	filter.filter.mask[1] = 0x00;   // Don't care
	
	// Don't filter on any other fields
	for (int i = 2; i < DMX_FILTER_SIZE; i++) {
		filter.filter.filter[i] = 0x00;
		filter.filter.mask[i] = 0x00;
	}
	
	// Filter options
	filter.flags = DMX_IMMEDIATE_START;   // Removed DMX_CHECK_CRC
	filter.timeout = 0; // No timeout at the driver level
	
	// Set the filter
	if (ioctl(fd, DMX_SET_FILTER, &filter) == -1) {
		DEBUG_PRINT("Failed to set filter: %s\n", strerror(errno));
		return -1;
	}
	
	return 0;
}

// Function to open a demux device and set up filtering
static PyObject* dvbreader_open(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	char *demuxer;
	int pid, table_id, mask, feid;
	int fd;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "siiii", &demuxer, &pid, &table_id, &mask, &feid))
		return NULL;
	
	// Open the demux device
	fd = open(demuxer, O_RDWR);
	if (fd < 0) {
		DEBUG_PRINT("Failed to open demuxer '%s': %s\n", demuxer, strerror(errno));
		PyErr_SetFromErrno(PyExc_IOError);
		return PyLong_FromLong(-1);
	}
	
	// Set up the filter
	if (setup_filter(fd, pid, table_id, mask, feid) < 0) {
		close(fd);
		PyErr_SetFromErrno(PyExc_IOError);
		return PyLong_FromLong(-1);
	}
	
	// Return the file descriptor
	return PyLong_FromLong(fd);
}

// Function to close a demux device
static PyObject* dvbreader_close(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	int fd;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "i", &fd))
		return NULL;
	
	// Close the file descriptor
	if (fd >= 0) {
		close(fd);
	}
	
	Py_RETURN_NONE;
}

// Helper function to parse a section header
static int parse_header(unsigned char *data, int len, PyObject **header_dict) {
	unsigned char table_id;
	unsigned short section_length;
	unsigned short table_id_ext = 0;
	unsigned char version_number = 0;
	unsigned char section_number = 0;
	unsigned char last_section_number = 0;
	unsigned short transport_stream_id = 0;
	unsigned short original_network_id = 0;
	
	// Check basic length
	if (len < SECTION_HEADER_LENGTH) {
		DEBUG_PRINT("Section too short for header: %d bytes\n", len);
		return -1;
	}
	
	// Get table ID and section length
	table_id = data[0];
	section_length = ((data[1] & 0x0f) << 8) | data[2];
	
	// Check section length
	if (section_length > MAX_SECTION_SIZE - SECTION_HEADER_LENGTH) {
		DEBUG_PRINT("Section length too large: %d bytes\n", section_length);
		return -1;
	}
	
	// Check total length
	if (len < SECTION_HEADER_LENGTH + section_length) {
		DEBUG_PRINT("Data too short for section: have %d, need %d bytes\n", 
				   len, SECTION_HEADER_LENGTH + section_length);
		return -1;
	}
	
	// For most table types, there's additional header info
	if (len >= 8) {
		table_id_ext = (data[3] << 8) | data[4];
		version_number = (data[5] >> 1) & 0x1f;
		section_number = data[6];
		last_section_number = data[7];
		
		// For SDT and NIT, there's even more header info
		if ((table_id == 0x42 || table_id == 0x46) && len >= 10) {  // SDT
			transport_stream_id = table_id_ext;
			original_network_id = (data[8] << 8) | data[9];
		} else if ((table_id == 0x40 || table_id == 0x41) && len >= 10) {  // NIT
			original_network_id = table_id_ext;
		}
	}
	
	// Create header dictionary
	*header_dict = PyDict_New();
	if (!*header_dict) {
		return -1;
	}
	
	// Add header fields to dictionary
	PyDict_SetItemString(*header_dict, "table_id", PyLong_FromLong(table_id));
	PyDict_SetItemString(*header_dict, "section_length", PyLong_FromLong(section_length));
	PyDict_SetItemString(*header_dict, "table_id_ext", PyLong_FromLong(table_id_ext));
	PyDict_SetItemString(*header_dict, "version_number", PyLong_FromLong(version_number));
	PyDict_SetItemString(*header_dict, "section_number", PyLong_FromLong(section_number));
	PyDict_SetItemString(*header_dict, "last_section_number", PyLong_FromLong(last_section_number));
	
	// For SDT/NIT add additional identifiers
	if (table_id == 0x42 || table_id == 0x46) {  // SDT
		PyDict_SetItemString(*header_dict, "transport_stream_id", PyLong_FromLong(transport_stream_id));
		PyDict_SetItemString(*header_dict, "original_network_id", PyLong_FromLong(original_network_id));
	} else if (table_id == 0x40 || table_id == 0x41) {  // NIT
		PyDict_SetItemString(*header_dict, "network_id", PyLong_FromLong(table_id_ext));
	}
	
	return section_length;
}

// Helper function to parse a descriptor (used in various tables)
// Currently unused, but kept for future extensibility
// Enhanced descriptor parsing
static PyObject* parse_descriptor(unsigned char *data, int len) {
	if (len < 2) {
		DEBUG_PRINT("Descriptor too short: %d bytes\n", len);
		Py_RETURN_NONE;
	}
	
	unsigned char descriptor_tag = data[0];
	unsigned char descriptor_length = data[1];
	
	DEBUG_PRINT("Processing descriptor tag 0x%02x, length %d\n", descriptor_tag, descriptor_length);
	
	if (len < 2 + descriptor_length) {
		DEBUG_PRINT("Descriptor buffer too short: have %d, need %d\n", len, 2 + descriptor_length);
		Py_RETURN_NONE;
	}
	
	PyObject *descriptor = PyDict_New();
	PyDict_SetItemString(descriptor, "descriptor_tag", PyLong_FromLong(descriptor_tag));
	PyDict_SetItemString(descriptor, "descriptor_length", PyLong_FromLong(descriptor_length));
	
	// Process specific descriptor types
	switch (descriptor_tag) {
		case 0x48: // Service descriptor
			DEBUG_PRINT("Found service descriptor\n");
			break;
		case 0x41: // Service list descriptor
			DEBUG_PRINT("Found service list descriptor\n");
			break;
		case 0x43: // Satellite delivery system descriptor
			DEBUG_PRINT("Found satellite delivery descriptor\n");
			break;
		case 0x5A: // Terrestrial delivery system descriptor 
			DEBUG_PRINT("Found terrestrial delivery descriptor\n");
			break;
		case 0x62: // Frequency list descriptor
			DEBUG_PRINT("Found frequency list descriptor\n");
			break;
		default:
			DEBUG_PRINT("Unhandled descriptor type: 0x%02x\n", descriptor_tag);
	}
	
	return descriptor;
}

// Convert DVB encoded text to UTF-8
static PyObject* convert_dvb_text(unsigned char *data, int len) {
	// Simplified text conversion - just assumes ISO-8859-1 for now
	// Use PyUnicode_DecodeLatin1 instead of PyUnicode_DecodeISO8859_1 to avoid warnings
	return PyUnicode_DecodeLatin1((const char*)data, len, "replace");
}

// Helper function to read SDT service information
static int parse_service_descriptor(unsigned char *data, int len, PyObject *service) {
	if (len < 5) {
		return -1;
	}
	
	unsigned char service_type = data[2];
	unsigned char service_provider_length = data[3];
	
	if (len < 4 + service_provider_length + 1) {
		return -1;
	}
	
	unsigned char service_name_length = data[4 + service_provider_length];
	
	if (len < 5 + service_provider_length + service_name_length) {
		return -1;
	}
	
	PyDict_SetItemString(service, "service_type", PyLong_FromLong(service_type));
	
	// Convert provider name
	if (service_provider_length > 0) {
		PyObject *provider_name = convert_dvb_text(&data[4], service_provider_length);
		PyDict_SetItemString(service, "provider_name", provider_name);
		Py_DECREF(provider_name);
	} else {
		PyDict_SetItemString(service, "provider_name", PyUnicode_FromString(""));
	}
	
	// Convert service name
	if (service_name_length > 0) {
		PyObject *service_name = convert_dvb_text(&data[5 + service_provider_length], service_name_length);
		PyDict_SetItemString(service, "service_name", service_name);
		Py_DECREF(service_name);
	} else {
		PyDict_SetItemString(service, "service_name", PyUnicode_FromString(""));
	}
	
	return 0;
}

// Parse SDT (Service Description Table)
static int parse_sdt_content(unsigned char *data, int len, PyObject *content_list) {
	DEBUG_PRINT("Parsing SDT content, length: %d bytes\n", len);
	int pos = 11; // Start after the fixed header (SDT header is 11 bytes)
	int services_found = 0;
	
	if (len < 11) {
		DEBUG_PRINT("SDT too short for header, length: %d\n", len);
		return -1;
	}
	
	// Log SDT header info
	unsigned short transport_stream_id = (data[3] << 8) | data[4];
	unsigned short original_network_id = (data[8] << 8) | data[9];
	DEBUG_PRINT("SDT for TSID: %d, Original Network ID: %d\n", 
		transport_stream_id, original_network_id);
	
	while (pos + 4 < len) {
		// Service ID is a 16-bit value
		unsigned short service_id = (data[pos] << 8) | data[pos + 1];
		unsigned char running_status = (data[pos + 3] >> 5) & 0x07;
		unsigned char free_ca = (data[pos + 3] >> 4) & 0x01;
		unsigned short descriptors_loop_length = ((data[pos + 3] & 0x0f) << 8) | data[pos + 4];
		
		DEBUG_PRINT("Found service ID: %d, status: %d, free_ca: %d, desc length: %d\n", 
			service_id, running_status, free_ca, descriptors_loop_length);
		
		pos += 5;
		
		if (pos + descriptors_loop_length > len) {
			DEBUG_PRINT("Descriptor loop exceeds buffer length: %d > %d\n", 
				pos + descriptors_loop_length, len);
			// Don't return error, try to continue with the services found so far
			break;
		}
		
		// Create service entry
		PyObject *service = PyDict_New();
		PyDict_SetItemString(service, "service_id", PyLong_FromLong(service_id));
		PyDict_SetItemString(service, "running_status", PyLong_FromLong(running_status));
		PyDict_SetItemString(service, "free_ca", PyLong_FromLong(free_ca));
		
		// Set defaults (in case no service descriptor is found)
		PyDict_SetItemString(service, "service_name", PyUnicode_FromString(""));
		PyDict_SetItemString(service, "provider_name", PyUnicode_FromString(""));
		PyDict_SetItemString(service, "service_type", PyLong_FromLong(0));
		
		// Parse descriptors
		int descriptors_end = pos + descriptors_loop_length;
		int service_descriptor_found = 0;
		
		while (pos < descriptors_end && pos < len) {
			if (pos + 2 > len) {
				DEBUG_PRINT("Descriptor header truncated\n");
				break; // Descriptor header truncated
			}
			
			unsigned char descriptor_tag = data[pos];
			unsigned char descriptor_length = data[pos + 1];
			
			DEBUG_PRINT("Processing descriptor tag: 0x%02x, length: %d\n", 
				descriptor_tag, descriptor_length);
			
			if (pos + 2 + descriptor_length > len) {
				DEBUG_PRINT("Descriptor exceeds buffer length\n");
				break; // Descriptor data truncated
			}
			
			// Service descriptor (0x48)
			if (descriptor_tag == 0x48) {
				DEBUG_PRINT("Found service descriptor for service ID: %d\n", service_id);
				service_descriptor_found = 1;
				if (parse_service_descriptor(&data[pos], descriptor_length + 2, service) < 0) {
					DEBUG_PRINT("Error parsing service descriptor\n");
				}
			}
			
			pos += descriptor_length + 2;
		}
		
		// Even if no service descriptor was found, we still add the service
		// since the ID and other basic info is still useful
		PyList_Append(content_list, service);
		Py_DECREF(service);
		services_found++;
	}
	
	DEBUG_PRINT("SDT parsing complete, found %d services\n", services_found);
	return (services_found > 0) ? 0 : -1;
}

// Parse NIT (Network Information Table)
static int parse_nit_content(unsigned char *data, int len, PyObject *content_list) {
	int pos = 10; // Start after fixed header (NIT header is 10 bytes)
	
	// First, skip network descriptors
	if (pos + 2 > len) return -1;
	unsigned short network_descriptors_length = ((data[pos] & 0x0f) << 8) | data[pos + 1];
	pos += 2 + network_descriptors_length;
	
	// Now, get transport stream loop length
	if (pos + 2 > len) return -1;
	unsigned short transport_stream_loop_length = ((data[pos] & 0x0f) << 8) | data[pos + 1];
	pos += 2;
	
	// Parse transport streams
	int ts_loop_end = pos + transport_stream_loop_length;
	while (pos < ts_loop_end && pos < len) {
		if (pos + 6 > len) break;
		
		unsigned short transport_stream_id = (data[pos] << 8) | data[pos + 1];
		unsigned short original_network_id = (data[pos + 2] << 8) | data[pos + 3];
		unsigned short descriptors_loop_length = ((data[pos + 4] & 0x0f) << 8) | data[pos + 5];
		
		pos += 6;
		
		// Process descriptors for this transport stream
		int descriptors_end = pos + descriptors_loop_length;
		while (pos < descriptors_end && pos < len) {
			if (pos + 2 > len) break;
			
			unsigned char descriptor_tag = data[pos];
			unsigned char descriptor_length = data[pos + 1];
			
			// Satellite delivery system descriptor (0x43)
			if (descriptor_tag == 0x43 && descriptor_length >= 11) {
				PyObject *transponder = PyDict_New();
				
				// Add transport_stream_id and original_network_id
				PyDict_SetItemString(transponder, "transport_stream_id", 
									PyLong_FromLong(transport_stream_id));
				PyDict_SetItemString(transponder, "original_network_id", 
									PyLong_FromLong(original_network_id));
				PyDict_SetItemString(transponder, "descriptor_tag", 
									PyLong_FromLong(descriptor_tag));
				
				// Satellite delivery specific info
				unsigned int frequency = 
					((data[pos + 2] >> 4) * 10000000) +
					((data[pos + 2] & 0x0f) * 1000000) +
					((data[pos + 3] >> 4) * 100000) +
					((data[pos + 3] & 0x0f) * 10000) +
					((data[pos + 4] >> 4) * 1000) +
					((data[pos + 4] & 0x0f) * 100) +
					((data[pos + 5] >> 4) * 10) +
					(data[pos + 5] & 0x0f);
				
				unsigned short orbital_position = (data[pos + 6] << 8) | data[pos + 7];
				unsigned char west_east_flag = (data[pos + 8] >> 7) & 0x01;
				unsigned char polarization = (data[pos + 8] >> 5) & 0x03;
				unsigned char modulation_system = (data[pos + 8] >> 4) & 0x01; // 0=DVB-S, 1=DVB-S2
				unsigned char modulation_type = (data[pos + 8] >> 2) & 0x03;
				
				unsigned int symbol_rate = 
					((data[pos + 9] >> 4) * 100000) +
					((data[pos + 9] & 0x0f) * 10000) +
					((data[pos + 10] >> 4) * 1000) +
					((data[pos + 10] & 0x0f) * 100) +
					((data[pos + 11] >> 4) * 10) +
					(data[pos + 11] & 0x0f);
				
				unsigned char fec_inner = data[pos + 12] & 0x0f;
				
				// Add the values to the transponder dictionary
				PyDict_SetItemString(transponder, "frequency", PyLong_FromLong(frequency * 10));
				PyDict_SetItemString(transponder, "orbital_position", PyLong_FromLong(orbital_position));
				PyDict_SetItemString(transponder, "west_east_flag", PyLong_FromLong(west_east_flag));
				PyDict_SetItemString(transponder, "polarization", PyLong_FromLong(polarization));
				PyDict_SetItemString(transponder, "modulation_system", PyLong_FromLong(modulation_system));
				PyDict_SetItemString(transponder, "modulation_type", PyLong_FromLong(modulation_type));
				PyDict_SetItemString(transponder, "symbol_rate", PyLong_FromLong(symbol_rate * 100));
				PyDict_SetItemString(transponder, "fec_inner", PyLong_FromLong(fec_inner));
				
				// Add transponder to content list
				PyList_Append(content_list, transponder);
				Py_DECREF(transponder);
			}
			
			pos += descriptor_length + 2;
		}
	}
	
	return 0;
}

// Improved wait strategy with better timeout handling and timeouts in ms
static ssize_t read_with_timeout(int fd, void *buf, size_t count, int timeout_ms) {
	fd_set rset;
	struct timeval tv;
	int ret;
	
	// Prepare file descriptor set
	FD_ZERO(&rset);
	FD_SET(fd, &rset);
	
	// Set timeout
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	
	// Wait for data
	ret = select(fd + 1, &rset, NULL, NULL, &tv);
	
	if (ret < 0) {
		// Error in select
		return -1;
	} else if (ret == 0) {
		// Timeout
		return 0;
	}
	
	// Data available, read it
	return read(fd, buf, count);
}

// Read a section from the demux
static PyObject* read_section(int fd, uint8_t table_id, uint8_t table_id_mask, uint8_t next_table_id) {
	unsigned char buffer[MAX_SECTION_SIZE];
	ssize_t bytes_read;
	int section_length;
	PyObject *result = NULL, *header = NULL, *content = NULL;
	
	// Add outer retry loop with reduced count for better performance
	int outer_retry = 2;
	
	while (outer_retry > 0) {
		// Reset inner retry count each time through outer loop
		int retry_count = g_retry_count;
		
		while (retry_count > 0) {
			// Read the data with timeout
			bytes_read = read_with_timeout(fd, buffer, sizeof(buffer), g_section_timeout_ms);
			
			if (bytes_read <= 0) {
				// Timeout or error, retry
				retry_count--;
				continue;
			}
			
			// Verify table ID
			if ((buffer[0] & table_id_mask) != (table_id & table_id_mask) && 
				(next_table_id == 0 || buffer[0] != next_table_id)) {
				// Wrong table ID, retry
				retry_count--;
				continue;
			}
			
			// Parse the section header
			section_length = parse_header(buffer, bytes_read, &header);
			if (section_length < 0) {
				DEBUG_PRINT("Error parsing section header\n");
				retry_count--;
				if (header) {
					Py_DECREF(header);
					header = NULL;
				}
				continue;
			}
			
			// Create content list
			content = PyList_New(0);
			if (!content) {
				Py_DECREF(header);
				return NULL;
			}
			
			// Parse section content based on table ID
			DEBUG_PRINT("Processing section with table ID: 0x%02x\n", buffer[0]);
			
			if (buffer[0] == 0x42 || buffer[0] == 0x46) {  // SDT (actual or other)
				DEBUG_PRINT("Found SDT table (0x%02x), parsing content\n", buffer[0]);
				if (parse_sdt_content(buffer, section_length + 3, content) < 0) {
					DEBUG_PRINT("Error parsing SDT content\n");
					retry_count--;
					Py_DECREF(header);
					Py_DECREF(content);
					continue;
				}
			} else if (buffer[0] == 0x40 || buffer[0] == 0x41) {  // NIT (actual or other)
				DEBUG_PRINT("Found NIT table (0x%02x), parsing content\n", buffer[0]);
				if (parse_nit_content(buffer, section_length + 3, content) < 0) {
					DEBUG_PRINT("Error parsing NIT content\n");
					retry_count--;
					Py_DECREF(header);
					Py_DECREF(content);
					continue;
				}
			} else if (buffer[0] == 0x4E || buffer[0] == 0x4F || // EIT (present/following - actual or other)
					  (buffer[0] >= 0x50 && buffer[0] <= 0x6F)) { // EIT (schedule - actual or other)
				// For EIT tables, we just extract the basic header info
				// This helps with service detection for channels that only broadcast EIT
				DEBUG_PRINT("Found EIT table (0x%02x), adding basic info\n", buffer[0]);
				
				// Add the table information to the content to signal we found something
				PyObject *eit_info = PyDict_New();
				PyDict_SetItemString(eit_info, "table_id", PyLong_FromLong(buffer[0]));
				PyDict_SetItemString(eit_info, "service_id", PyLong_FromLong((buffer[3] << 8) | buffer[4]));
				PyDict_SetItemString(eit_info, "ts_id", PyLong_FromLong((buffer[8] << 8) | buffer[9]));
				PyDict_SetItemString(eit_info, "original_network_id", PyLong_FromLong((buffer[10] << 8) | buffer[11]));
				
				PyList_Append(content, eit_info);
				Py_DECREF(eit_info);
			} else if (buffer[0] == 0x00) { // PAT (Program Association Table)
				// For PAT tables, extract the program information
				DEBUG_PRINT("Found PAT table (0x00), adding program info\n");
				
				if (len >= 8) {  // Minimum length for PAT
					unsigned short ts_id = (buffer[3] << 8) | buffer[4];
					DEBUG_PRINT("PAT for TSID: %d\n", ts_id);
					
					// Skip header and process program entries
					int pos = 8;  // Start after the fixed header
					
					// Process all program entries
					while (pos + 4 <= section_length + 3) {
						unsigned short program_number = (buffer[pos] << 8) | buffer[pos + 1];
						unsigned short pid = ((buffer[pos + 2] & 0x1F) << 8) | buffer[pos + 3];
						
						if (program_number != 0) {  // Skip network_PID entry (program_number == 0)
							DEBUG_PRINT("Found program: %d, PMT PID: 0x%04x\n", program_number, pid);
							
							// Create a service entry with the program info
							PyObject *service = PyDict_New();
							PyDict_SetItemString(service, "service_id", PyLong_FromLong(program_number));
							PyDict_SetItemString(service, "pmt_pid", PyLong_FromLong(pid));
							PyDict_SetItemString(service, "from_pat", PyLong_FromLong(1));
							
							// Add default values that would normally come from SDT
							PyDict_SetItemString(service, "service_name", PyUnicode_FromFormat("Service %d", program_number));
							PyDict_SetItemString(service, "provider_name", PyUnicode_FromString(""));
							PyDict_SetItemString(service, "service_type", PyLong_FromLong(1));  // Default to TV service
							
							PyList_Append(content, service);
							Py_DECREF(service);
						}
						
						pos += 4;  // Move to next program entry
					}
				}
			}
			
			// Create result dictionary
			result = PyDict_New();
			PyDict_SetItemString(result, "header", header);
			PyDict_SetItemString(result, "content", content);
			
			Py_DECREF(header);
			Py_DECREF(content);
			
			// Success, return the result
			return result;
		}
		
		// If we get here, all inner retries failed
		// Sleep briefly before trying again with outer loop
		struct timespec ts;
		ts.tv_sec = 0;
		ts.tv_nsec = 250000000;  // 250ms - reduced from 500ms for better UI responsiveness
		DEBUG_PRINT("All inner retries failed, sleeping 250ms before outer retry %d of 3\n", 4 - outer_retry);
		nanosleep(&ts, NULL);
		
		outer_retry--;
	}
	
	// All retries (inner and outer) failed
	Py_RETURN_NONE;
}

// Function to read SDT (Service Description Table)
static PyObject* dvbreader_read_sdt(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	int fd;
	int table_id, table_id_mask = 0;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "iii", &fd, &table_id, &table_id_mask))
		return NULL;
	
	// Try reading SDT first
	PyObject *result = read_section(fd, table_id, table_id_mask, 0);
	
	// If SDT didn't return any services or returned None, try reading PAT
	if (result == Py_None || result == NULL) {
		DEBUG_PRINT("SDT read failed, trying PAT (0x00) instead\n");
		
		// We need to reopen the demux for PAT
		if (setup_filter(fd, 0x00, 0x00, 0xff, 0) == 0) {
			DEBUG_PRINT("Demux refiltered for PAT\n");
			// Try to read PAT
			result = read_section(fd, 0x00, 0xff, 0);
		}
	} else {
		PyObject *content = PyDict_GetItemString(result, "content");
		if (content != NULL && PyList_Size(content) == 0) {
			DEBUG_PRINT("SDT returned empty content, trying PAT\n");
			Py_DECREF(result);
			
			// We need to reopen the demux for PAT
			if (setup_filter(fd, 0x00, 0x00, 0xff, 0) == 0) {
				DEBUG_PRINT("Demux refiltered for PAT\n");
				// Try to read PAT
				result = read_section(fd, 0x00, 0xff, 0);
			} else {
				result = Py_None;
				Py_INCREF(result);
			}
		}
	}
	
	return result;
}

// Function to read NIT (Network Information Table)
static PyObject* dvbreader_read_nit(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	int fd;
	int table_id, next_table_id;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "iii", &fd, &table_id, &next_table_id))
		return NULL;
	
	// Read the section
	return read_section(fd, table_id, 0xff, next_table_id);
}

// Function to read BAT (Bouquet Association Table)
static PyObject* dvbreader_read_bat(PyObject *self, PyObject *args) {
	int fd;
	int table_id, bouquet_id;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "iii", &fd, &table_id, &bouquet_id))
		return NULL;
	
	UNUSED(bouquet_id); // Mark as unused to avoid warning
	
	// For simplicity, we're just calling the generic read_section, but in a real
	// implementation, we'd have specialized handling for BAT
	return read_section(fd, table_id, 0xff, 0);
}

// Function to read FastScan table
static PyObject* dvbreader_read_fastscan(PyObject *self, PyObject *args) {
	int fd;
	int table_id, bouquet_id;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "iii", &fd, &table_id, &bouquet_id))
		return NULL;
	
	UNUSED(bouquet_id); // Mark as unused to avoid warning
	
	// For simplicity, we're just calling the generic read_section, but in a real
	// implementation, we'd have specialized handling for FastScan
	return read_section(fd, table_id, 0xff, 0);
}

// Function to read a generic DVB table (for compatibility)
static PyObject* dvbreader_read_ts(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	int fd;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "i", &fd))
		return NULL;
	
	// Create a buffer to read the TS packet
	unsigned char buffer[TS_PACKET_SIZE];
	ssize_t bytes_read;
	
	// Read the data
	bytes_read = read(fd, buffer, sizeof(buffer));
	if (bytes_read <= 0) {
		Py_RETURN_NONE;
	}
	
	// Return the raw data as bytes
	return PyBytes_FromStringAndSize((char *)buffer, bytes_read);
}

// Function to set global timeout settings
static PyObject* dvbreader_set_timeouts(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	int section_timeout_ms, complete_timeout_ms;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "ii", &section_timeout_ms, &complete_timeout_ms))
		return NULL;
	
	// Set global timeout values
	g_section_timeout_ms = section_timeout_ms;
	g_complete_timeout_ms = complete_timeout_ms;
	
	Py_RETURN_NONE;
}

// Function to set global retry count
static PyObject* dvbreader_set_retry_count(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	int retry_count;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "i", &retry_count))
		return NULL;
	
	// Set global retry count
	g_retry_count = retry_count;
	
	Py_RETURN_NONE;
}

// Functions needed for parsing tables but not directly exposed

// Function to parse SDT header (for compatibility with original API)
static PyObject* dvbreader_parse_header(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	PyObject *byteArray;
	Py_buffer buffer;
	PyObject *header = NULL;
	int result;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "O", &byteArray))
		return NULL;
	
	// Get buffer
	if (PyObject_GetBuffer(byteArray, &buffer, PyBUF_SIMPLE) < 0)
		return NULL;
	
	// Parse header
	result = parse_header(buffer.buf, buffer.len, &header);
	
	// Release buffer
	PyBuffer_Release(&buffer);
	
	if (result < 0) {
		Py_RETURN_NONE;
	}
	
	return header;
}

// Function to parse NIT header (for compatibility with original API)
static PyObject* dvbreader_parse_header_nit(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	// For simplicity, just calls parse_header
	return dvbreader_parse_header(self, args);
}

// Function to parse BAT header (for compatibility with original API)
static PyObject* dvbreader_parse_header_bat(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	// For simplicity, just calls parse_header
	return dvbreader_parse_header(self, args);
}

// Function to parse SDT content (for compatibility with original API)
static PyObject* dvbreader_parse_sdt(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	PyObject *byteArray;
	Py_buffer buffer;
	PyObject *content = NULL;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "O", &byteArray))
		return NULL;
	
	// Get buffer
	if (PyObject_GetBuffer(byteArray, &buffer, PyBUF_SIMPLE) < 0)
		return NULL;
	
	// Create content list
	content = PyList_New(0);
	if (!content) {
		PyBuffer_Release(&buffer);
		return NULL;
	}
	
	// Parse content
	if (parse_sdt_content(buffer.buf, buffer.len, content) < 0) {
		Py_DECREF(content);
		PyBuffer_Release(&buffer);
		Py_RETURN_NONE;
	}
	
	// Release buffer
	PyBuffer_Release(&buffer);
	
	return content;
}

// Function to parse NIT content (for compatibility with original API)
static PyObject* dvbreader_parse_nit(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	PyObject *byteArray;
	Py_buffer buffer;
	PyObject *content = NULL;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "O", &byteArray))
		return NULL;
	
	// Get buffer
	if (PyObject_GetBuffer(byteArray, &buffer, PyBUF_SIMPLE) < 0)
		return NULL;
	
	// Create content list
	content = PyList_New(0);
	if (!content) {
		PyBuffer_Release(&buffer);
		return NULL;
	}
	
	// Parse content
	if (parse_nit_content(buffer.buf, buffer.len, content) < 0) {
		Py_DECREF(content);
		PyBuffer_Release(&buffer);
		Py_RETURN_NONE;
	}
	
	// Release buffer
	PyBuffer_Release(&buffer);
	
	return content;
}

// Function to parse BAT content (for compatibility with original API)
static PyObject* dvbreader_parse_bat(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	UNUSED(args); // Mark as unused to avoid warning
	// This would be a full implementation in the actual module
	PyObject *content = PyList_New(0);
	return content;
}

// Function to parse FastScan content (for compatibility with original API)
static PyObject* dvbreader_parse_fastscan(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	UNUSED(args); // Mark as unused to avoid warning
	// This would be a full implementation in the actual module
	PyObject *content = PyList_New(0);
	return content;
}

// Generic table parser for any table type
static PyObject* dvbreader_parse_table(PyObject *self, PyObject *args) {
	UNUSED(self); // Mark as unused to avoid warning
	PyObject *byteArray;
	int table_id;
	Py_buffer buffer;
	PyObject *result = NULL, *header = NULL, *content = NULL;
	
	// Parse arguments
	if (!PyArg_ParseTuple(args, "Oi", &byteArray, &table_id))
		return NULL;
	
	// Get buffer
	if (PyObject_GetBuffer(byteArray, &buffer, PyBUF_SIMPLE) < 0)
		return NULL;
	
	// Parse header
	int section_length = parse_header(buffer.buf, buffer.len, &header);
	if (section_length < 0) {
		PyBuffer_Release(&buffer);
		Py_RETURN_NONE;
	}
	
	// Create content list
	content = PyList_New(0);
	if (!content) {
		Py_DECREF(header);
		PyBuffer_Release(&buffer);
		return NULL;
	}
	
	// Parse content based on table ID
	if (table_id == 0x42 || table_id == 0x46) {  // SDT
		parse_sdt_content(buffer.buf, buffer.len, content);
	} else if (table_id == 0x40 || table_id == 0x41) {  // NIT
		parse_nit_content(buffer.buf, buffer.len, content);
	}
	// Add more table types as needed
	
	// Create result dictionary
	result = PyDict_New();
	PyDict_SetItemString(result, "header", header);
	PyDict_SetItemString(result, "content", content);
	
	Py_DECREF(header);
	Py_DECREF(content);
	
	// Release buffer
	PyBuffer_Release(&buffer);
	
	return result;
}

// Module method definitions
static PyMethodDef DvbreaderMethods[] = {
	{"open", dvbreader_open, METH_VARARGS, "Open a demux device for filtering."},
	{"close", dvbreader_close, METH_VARARGS, "Close a demux device."},
	{"read_sdt", dvbreader_read_sdt, METH_VARARGS, "Read SDT section."},
	{"read_nit", dvbreader_read_nit, METH_VARARGS, "Read NIT section."},
	{"read_bat", dvbreader_read_bat, METH_VARARGS, "Read BAT section."},
	{"read_fastscan", dvbreader_read_fastscan, METH_VARARGS, "Read FastScan section."},
	{"read_ts", dvbreader_read_ts, METH_VARARGS, "Read generic TS packet."},
	{"set_timeouts", dvbreader_set_timeouts, METH_VARARGS, "Set section and complete timeouts."},
	{"set_retry_count", dvbreader_set_retry_count, METH_VARARGS, "Set retry count for failed reads."},
	{"parse_header", dvbreader_parse_header, METH_VARARGS, "Parse section header."},
	{"parse_header_nit", dvbreader_parse_header_nit, METH_VARARGS, "Parse NIT header."},
	{"parse_header_bat", dvbreader_parse_header_bat, METH_VARARGS, "Parse BAT header."},
	{"parse_sdt", dvbreader_parse_sdt, METH_VARARGS, "Parse SDT content."},
	{"parse_nit", dvbreader_parse_nit, METH_VARARGS, "Parse NIT content."},
	{"parse_bat", dvbreader_parse_bat, METH_VARARGS, "Parse BAT content."},
	{"parse_fastscan", dvbreader_parse_fastscan, METH_VARARGS, "Parse FastScan content."},
	{"parse_table", dvbreader_parse_table, METH_VARARGS, "Parse any table type."},
	{NULL, NULL, 0, NULL} // Sentinel
};

// Module definition
static struct PyModuleDef dvbreadermodule = {
	PyModuleDef_HEAD_INIT,
	"dvbreader",
	"Improved DVB Stream Reader", // Module docstring
	-1, // Module keeps state in global variables
	DvbreaderMethods,
	NULL, // m_slots - initialize explicitly to NULL
	NULL, // m_traverse
	NULL, // m_clear
	NULL  // m_free
};

// Module initialization function
PyMODINIT_FUNC PyInit_dvbreader(void) {
	return PyModule_Create(&dvbreadermodule);
}
