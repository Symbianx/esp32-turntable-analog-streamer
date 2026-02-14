#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "config_schema.h"

class NVSConfig {
public:
    // Initialize NVS flash
    static bool init();
    
    // Load device config from NVS
    // Returns false and loads factory defaults if config is invalid/missing
    static bool load(DeviceConfig *config);
    
    // Save device config to NVS with CRC32 validation
    static bool save(const DeviceConfig *config);
    
    // Load factory default configuration
    static bool load_factory_defaults(DeviceConfig *config);
    
    // Erase all config from NVS (factory reset)
    static bool erase();
};

#endif // NVS_CONFIG_H
