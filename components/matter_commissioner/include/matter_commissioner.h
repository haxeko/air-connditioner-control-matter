#pragma once

/*
 * Fabric B commissioner: pairs Matter sensors that the user shared from another ecosystem
 * (Multi-Admin, Enhanced Commissioning Method) into the fabric owned by this device, then lists
 * their Temperature Measurement endpoints.
 *
 * All functions are callable from any task except the ISR; work is executed on the CHIP thread.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMMISSIONER_IDLE = 0,
    COMMISSIONER_DISCOVERING,   /* DNS-SD search for the commissionable node (_matterc._udp)    */
    COMMISSIONER_PASE,          /* PASE secure session with the setup passcode                 */
    COMMISSIONER_COMMISSIONING, /* attestation, CSR/NOC, ACL, operational discovery, CASE       */
    COMMISSIONER_READING,       /* reading BasicInformation + Temperature Measurement endpoints */
    COMMISSIONER_DONE,
    COMMISSIONER_FAILED,
    COMMISSIONER_REMOVING,      /* sending RemoveFabric to a sensor                             */
} commissioner_state_t;

#define COMMISSIONER_MAX_ENDPOINTS 8

typedef struct {
    uint16_t endpoint;
    bool read_ok;
    bool value_null;      /* MeasuredValue == null (sensor reports "unknown")   */
    int16_t measured;     /* 0.01 degC                                          */
    bool has_min, has_max;
    int16_t min_measured, max_measured;
} commissioner_endpoint_t;

typedef struct {
    commissioner_state_t state;
    char stage[48];               /* last CHIP commissioning stage (English, from the SDK)       */
    char error[96];               /* human readable error (no secrets)                           */
    bool attestation_overridden;  /* user allowed an uncertified/test device                     */
    char attestation_note[64];
    uint64_t node_id;             /* node ID of the device in Fabric B                           */
    bool already_on_fabric;       /* device was already commissioned into Fabric B               */
    uint16_t vendor_id, product_id;
    char vendor_name[33], product_name[33], node_label[33];
    uint8_t endpoint_count;
    commissioner_endpoint_t endpoints[COMMISSIONER_MAX_ENDPOINTS];
    int64_t started_us, updated_us;
} commissioner_progress_t;

typedef struct {
    bool ready;                   /* commissioner initialised, Fabric B present           */
    char init_error[96];
    uint8_t fabric_index;
    uint64_t fabric_id;
    uint64_t compressed_fabric_id;
    uint64_t controller_node_id;
    uint16_t paa_count;
    uint16_t listen_port;
} commissioner_info_t;

/**
 * Must run on the CHIP thread immediately after esp_matter::start() (before any controller can
 * subscribe to the server): DeviceControllerFactory re-initialises the InteractionModelEngine
 * and aborts if read handlers already exist. `server_init_params` is the
 * chip::CommonCaseDeviceServerInitParams instance handed to esp_matter::set_server_init_params().
 */
esp_err_t matter_commissioner_init_on_chip_thread(void *server_init_params);

void matter_commissioner_get_info(commissioner_info_t *out);
void matter_commissioner_get_progress(commissioner_progress_t *out);
bool matter_commissioner_busy(void);

/**
 * Start on-network commissioning with a Matter onboarding payload: 11/21 digit manual pairing code
 * (dashes/spaces allowed) or "MT:" QR payload. Returns ESP_ERR_INVALID_ARG for a malformed code.
 * The code is never logged.
 */
esp_err_t matter_commissioner_start_pairing(const char *setup_code, bool allow_uncertified);
esp_err_t matter_commissioner_cancel(void);

/** Re-read BasicInformation and all Temperature Measurement endpoints of an already paired node. */
esp_err_t matter_commissioner_refresh(uint64_t node_id);

/**
 * Remove Fabric B from the sensor (RemoveFabric on the sensor's own fabric index; the sensor's
 * other fabrics, e.g. the original hub, are not touched). Result in the progress structure.
 */
esp_err_t matter_commissioner_remove_node(uint64_t node_id);

/** Validate an onboarding payload without starting anything (no CHIP thread needed). */
bool matter_commissioner_validate_code(const char *setup_code);

#ifdef __cplusplus
}
#endif
