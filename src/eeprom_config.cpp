#include "eeprom_config.h"
#include <EEPROM.h>

// ─── On-flash record layout ──────────────────────────────────────────────────
// Bumping EE_VERSION invalidates every previously stored record, so old boards
// fall back to defaults instead of misreading a shorter struct.

#define EE_MAGIC    0x554E5234UL    // "UNR4"
#define EE_VERSION  0x0001
#define EE_ADDRESS  0

struct StoredConfig {
    uint32_t magic;
    uint16_t version;
    uint8_t  useDhcp;
    uint8_t  reserved;              // keeps the IP array 4-byte aligned
    uint8_t  ip[4];
    uint8_t  gateway[4];
    uint8_t  subnet[4];
    uint8_t  dns[4];
    uint8_t  mac[6];
    char     sysName[SYS_NAME_SIZE];
    uint16_t crc;                   // CRC-16/CCITT over all preceding bytes
};

// ─── CRC-16/CCITT-FALSE ──────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint16_t recordCRC(const StoredConfig& rec) {
    return crc16((const uint8_t*)&rec, sizeof(StoredConfig) - sizeof(uint16_t));
}

// ─── Conversion between the RAM view and the flash record ────────────────────

static void toRecord(const NetworkConfig& cfg, StoredConfig& rec) {
    memset(&rec, 0, sizeof(rec));
    rec.magic    = EE_MAGIC;
    rec.version  = EE_VERSION;
    rec.useDhcp  = cfg.useDhcp ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        rec.ip[i]      = cfg.ip[i];
        rec.gateway[i] = cfg.gateway[i];
        rec.subnet[i]  = cfg.subnet[i];
        rec.dns[i]     = cfg.dns[i];
    }
    memcpy(rec.mac, cfg.mac, 6);
    strncpy(rec.sysName, cfg.sysName, SYS_NAME_SIZE - 1);
    rec.sysName[SYS_NAME_SIZE - 1] = '\0';
    rec.crc = recordCRC(rec);
}

static void fromRecord(const StoredConfig& rec, NetworkConfig& cfg) {
    cfg.useDhcp = (rec.useDhcp != 0);
    cfg.ip      = IPAddress(rec.ip[0],      rec.ip[1],      rec.ip[2],      rec.ip[3]);
    cfg.gateway = IPAddress(rec.gateway[0], rec.gateway[1], rec.gateway[2], rec.gateway[3]);
    cfg.subnet  = IPAddress(rec.subnet[0],  rec.subnet[1],  rec.subnet[2],  rec.subnet[3]);
    cfg.dns     = IPAddress(rec.dns[0],     rec.dns[1],     rec.dns[2],     rec.dns[3]);
    memcpy(cfg.mac, rec.mac, 6);
    memcpy(cfg.sysName, rec.sysName, SYS_NAME_SIZE);
    cfg.sysName[SYS_NAME_SIZE - 1] = '\0';
}

static bool readRecord(StoredConfig& rec) {
    EEPROM.get(EE_ADDRESS, rec);
    if (rec.magic != EE_MAGIC)          return false;
    if (rec.version != EE_VERSION)      return false;
    if (rec.crc != recordCRC(rec))      return false;
    // A name that is not NUL terminated means the record is not trustworthy.
    if (rec.sysName[SYS_NAME_SIZE - 1] != '\0') return false;
    if (rec.sysName[0] == '\0')         return false;
    return true;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void EepromConfig::begin() {
    Serial.print("[EEPROM] data-flash available: ");
    Serial.print(capacity());
    Serial.print(" bytes, record uses ");
    Serial.print(recordSize());
    Serial.println(" bytes");
}

void EepromConfig::defaults(NetworkConfig& cfg) {
    static const uint8_t defMac[6] = MAC_ADDR;
    cfg.useDhcp = DEFAULT_USE_DHCP;
    cfg.ip      = DEFAULT_IP;
    cfg.gateway = DEFAULT_GATEWAY;
    cfg.subnet  = DEFAULT_SUBNET;
    cfg.dns     = DEFAULT_DNS;
    memcpy(cfg.mac, defMac, 6);
    memset(cfg.sysName, 0, SYS_NAME_SIZE);
    strncpy(cfg.sysName, DEFAULT_SYS_NAME, SYS_NAME_SIZE - 1);
}

bool EepromConfig::load(NetworkConfig& cfg) {
    StoredConfig rec;
    if (!readRecord(rec)) {
        defaults(cfg);
        return false;
    }
    fromRecord(rec, cfg);

    // Guard against a record that is structurally valid but semantically junk.
    String why;
    if (!validate(cfg, why)) {
        Serial.println("[EEPROM] stored record rejected: " + why);
        defaults(cfg);
        return false;
    }
    return true;
}

bool EepromConfig::save(const NetworkConfig& cfg) {
    StoredConfig rec;
    toRecord(cfg, rec);
    EEPROM.put(EE_ADDRESS, rec);

    // Read back and re-verify — a data-flash write can fail silently.
    StoredConfig check;
    EEPROM.get(EE_ADDRESS, check);
    return memcmp(&rec, &check, sizeof(StoredConfig)) == 0;
}

bool EepromConfig::erase() {
    StoredConfig rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.magic = 0;                  // magic mismatch => treated as absent
    EEPROM.put(EE_ADDRESS, rec);
    return !isStored();
}

bool EepromConfig::isStored() {
    StoredConfig rec;
    return readRecord(rec);
}

size_t EepromConfig::capacity()  { return (size_t)EEPROM.length(); }
size_t EepromConfig::recordSize(){ return sizeof(StoredConfig); }

// ─── Parsing / formatting helpers ────────────────────────────────────────────

bool EepromConfig::parseIP(const char* s, IPAddress& out) {
    if (!s) return false;
    unsigned int a, b, c, d;
    char extra;
    // The trailing %c catches "1.2.3.4.5" and "1.2.3.4x".
    int n = sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra);
    if (n != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
    return true;
}

bool EepromConfig::setSysName(NetworkConfig& cfg, const char* name) {
    if (!name) return false;

    // Trim surrounding whitespace.
    while (*name == ' ' || *name == '\t') name++;
    size_t len = strlen(name);
    while (len > 0 && (name[len-1] == ' ' || name[len-1] == '\t')) len--;

    if (len == 0 || len >= SYS_NAME_SIZE) return false;

    // Keep it to characters that are legal in a DHCP host name.
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }

    memset(cfg.sysName, 0, SYS_NAME_SIZE);
    memcpy(cfg.sysName, name, len);
    return true;
}

bool EepromConfig::parseMAC(const char* s, uint8_t mac[6]) {
    if (!s) return false;
    unsigned int v[6];
    char sep[5];
    if (sscanf(s, "%2x%c%2x%c%2x%c%2x%c%2x%c%2x",
               &v[0], &sep[0], &v[1], &sep[1], &v[2], &sep[2],
               &v[3], &sep[3], &v[4], &sep[4], &v[5]) != 11) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (sep[i] != ':' && sep[i] != '-' && sep[i] != '.') return false;
    }
    if (v[0] & 0x01) return false;              // reject multicast MACs
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) return false;
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

void EepromConfig::formatMAC(const uint8_t mac[6], char* out, size_t outLen) {
    snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ─── Validation ──────────────────────────────────────────────────────────────

// A legal IPv4 mask is a run of 1 bits followed by a run of 0 bits.
static bool isContiguousMask(const IPAddress& m) {
    uint32_t v = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) |
                 ((uint32_t)m[2] << 8)  |  (uint32_t)m[3];
    if (v == 0) return false;
    return (v & (~v >> 1)) == 0 && (v & 0x80000000UL) != 0;
}

bool EepromConfig::validate(const NetworkConfig& cfg, String& why) {
    if (cfg.sysName[0] == '\0') { why = "system name is empty"; return false; }

    if (cfg.mac[0] & 0x01) { why = "MAC is a multicast address"; return false; }
    bool macZero = true;
    for (int i = 0; i < 6; i++) if (cfg.mac[i]) macZero = false;
    if (macZero) { why = "MAC is all zeroes"; return false; }

    // In DHCP mode the static fields are unused, so stop checking here.
    if (cfg.useDhcp) return true;

    if (cfg.ip[0] == 0)   { why = "IP address is unset"; return false; }
    if (cfg.ip[0] == 127) { why = "IP address is loopback"; return false; }
    if (cfg.ip[0] >= 224) { why = "IP address is multicast/reserved"; return false; }

    if (!isContiguousMask(cfg.subnet)) { why = "subnet mask is not a valid netmask"; return false; }

    // Host part must not be all-zeroes (network) or all-ones (broadcast).
    uint32_t ip   = ((uint32_t)cfg.ip[0] << 24) | ((uint32_t)cfg.ip[1] << 16) |
                    ((uint32_t)cfg.ip[2] << 8)  |  (uint32_t)cfg.ip[3];
    uint32_t mask = ((uint32_t)cfg.subnet[0] << 24) | ((uint32_t)cfg.subnet[1] << 16) |
                    ((uint32_t)cfg.subnet[2] << 8)  |  (uint32_t)cfg.subnet[3];
    uint32_t host = ip & ~mask;
    if (mask != 0xFFFFFFFFUL) {
        if (host == 0)      { why = "IP address is the network address"; return false; }
        if (host == ~mask)  { why = "IP address is the broadcast address"; return false; }
    }

    // A gateway of 0.0.0.0 means "no router", which is legitimate on an
    // isolated lab segment. Anything else must be reachable on-link.
    uint32_t gw = ((uint32_t)cfg.gateway[0] << 24) | ((uint32_t)cfg.gateway[1] << 16) |
                  ((uint32_t)cfg.gateway[2] << 8)  |  (uint32_t)cfg.gateway[3];
    if (gw != 0 && (gw & mask) != (ip & mask)) {
        why = "gateway is not on the same subnet as the IP address";
        return false;
    }

    return true;
}
