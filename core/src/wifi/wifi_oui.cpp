// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A compact table of OUIs (the first three octets of a MAC address, IEEE registry) of the access-point and client vendors most often seen in Wi-Fi
// scans, for the vendor column (best effort: an address that is not in the table shows no vendor).
#include "dect2/wifi/wifi_proto.h"

namespace dect2::wifi {

namespace {
struct Oui { uint32_t oui; const char* name; };
const Oui kOui[] = {
    {0x000393, "Apple"}, {0x000A27, "Apple"}, {0x000A95, "Apple"}, {0x000D93, "Apple"},
    {0x001124, "Apple"}, {0x001451, "Apple"}, {0x0016CB, "Apple"}, {0x0017F2, "Apple"},
    {0x0019E3, "Apple"}, {0x001B63, "Apple"}, {0x001CB3, "Apple"}, {0x001D4F, "Apple"},
    {0x001E52, "Apple"}, {0x001EC2, "Apple"}, {0x001FF3, "Apple"}, {0x0021E9, "Apple"},
    {0x002241, "Apple"}, {0x002312, "Apple"}, {0x00236C, "Apple"}, {0x0023DF, "Apple"},
    {0x002436, "Apple"}, {0x002500, "Apple"}, {0x00254B, "Apple"}, {0x0025BC, "Apple"},
    {0x002608, "Apple"}, {0x00264A, "Apple"}, {0x0026B0, "Apple"}, {0x0026BB, "Apple"},
    {0x3C0754, "Apple"}, {0x403004, "Apple"}, {0x70DEE2, "Apple"}, {0xACBC32, "Apple"},
    {0xF01898, "Apple"}, {0xF0DBF8, "Apple"}, {0x00000C, "Cisco"}, {0x000142, "Cisco"},
    {0x000143, "Cisco"}, {0x000163, "Cisco"}, {0x000164, "Cisco"}, {0x000196, "Cisco"},
    {0x000197, "Cisco"}, {0x0001C7, "Cisco"}, {0x0001C9, "Cisco"}, {0x000216, "Cisco"},
    {0x000217, "Cisco"}, {0x00024A, "Cisco"}, {0x00024B, "Cisco"}, {0x00027D, "Cisco"},
    {0x00027E, "Cisco"}, {0x0002B9, "Cisco"}, {0x0002BA, "Cisco"}, {0x0002FC, "Cisco"},
    {0x0002FD, "Cisco"}, {0x000331, "Cisco"}, {0x000332, "Cisco"}, {0x00180A, "Cisco Meraki"},
    {0xE0553D, "Cisco Meraki"}, {0xAC17C8, "Cisco Meraki"}, {0x881DFC, "Cisco Meraki"}, {0x001839, "Linksys"},
    {0x001217, "Linksys"}, {0x001310, "Linksys"}, {0x0014BF, "Linksys"}, {0x0016B6, "Linksys"},
    {0x001A70, "Linksys"}, {0x001C10, "Linksys"}, {0x001D7E, "Linksys"}, {0x001EE5, "Linksys"},
    {0x002129, "Linksys"}, {0x00226B, "Linksys"}, {0x002369, "Linksys"}, {0x000625, "Linksys"},
    {0x000C41, "Linksys"}, {0x000E08, "Linksys"}, {0x000F66, "Linksys"}, {0x50C7BF, "TP-Link"},
    {0x14CC20, "TP-Link"}, {0xF4F26D, "TP-Link"}, {0x1C3BF3, "TP-Link"}, {0x3C46D8, "TP-Link"},
    {0x54C80F, "TP-Link"}, {0x5C628B, "TP-Link"}, {0x6466B3, "TP-Link"}, {0x84160D, "TP-Link"},
    {0x98DAC4, "TP-Link"}, {0xA0F3C1, "TP-Link"}, {0xB0487A, "TP-Link"}, {0xC025E9, "TP-Link"},
    {0xD8071B, "TP-Link"}, {0xE894F6, "TP-Link"}, {0xEC086B, "TP-Link"}, {0xF81A67, "TP-Link"},
    {0x60E327, "TP-Link"}, {0x00095B, "Netgear"}, {0x00146C, "Netgear"}, {0x00184D, "Netgear"},
    {0x001B2F, "Netgear"}, {0x001E2A, "Netgear"}, {0x001F33, "Netgear"}, {0x00223F, "Netgear"},
    {0x0024B2, "Netgear"}, {0x0026F2, "Netgear"}, {0x20E52A, "Netgear"}, {0x2C3033, "Netgear"},
    {0xA02088, "Netgear"}, {0x00156D, "Ubiquiti"}, {0x002722, "Ubiquiti"}, {0x0418D6, "Ubiquiti"},
    {0x18E829, "Ubiquiti"}, {0x24A43C, "Ubiquiti"}, {0x44D9E7, "Ubiquiti"}, {0x687251, "Ubiquiti"},
    {0x74ACB9, "Ubiquiti"}, {0x802AA8, "Ubiquiti"}, {0xB4FBE4, "Ubiquiti"}, {0xDC9FDB, "Ubiquiti"},
    {0xE063DA, "Ubiquiti"}, {0xF09FC2, "Ubiquiti"}, {0xFCECDA, "Ubiquiti"}, {0x788A20, "Ubiquiti"},
    {0x245A4C, "Ubiquiti"}, {0x9C05D6, "Ubiquiti"}, {0x00040E, "AVM"}, {0x3810D5, "AVM"},
    {0xC80E14, "AVM"}, {0x2C3AFD, "AVM"}, {0x3431C4, "AVM"}, {0x444E6D, "AVM"},
    {0x7CFF4D, "AVM"}, {0xDC15C8, "AVM"}, {0xE8DF70, "AVM"}, {0x989BCB, "AVM"},
    {0x00E0FC, "Huawei"}, {0x001E10, "Huawei"}, {0x002568, "Huawei"}, {0x00259E, "Huawei"},
    {0x0034FE, "Huawei"}, {0x00664B, "Huawei"}, {0x04C06F, "Huawei"}, {0x1C1D67, "Huawei"},
    {0x20F3A3, "Huawei"}, {0x34CDBE, "Huawei"}, {0x48AD08, "Huawei"}, {0x5C7D5E, "Huawei"},
    {0x001E73, "ZTE"}, {0x002293, "ZTE"}, {0x0015EB, "ZTE"}, {0x344B50, "ZTE"},
    {0x4C09B4, "ZTE"}, {0x8C68C8, "ZTE"}, {0x000C6E, "ASUS"}, {0x001D60, "ASUS"},
    {0x001A92, "ASUS"}, {0x001E8C, "ASUS"}, {0x002354, "ASUS"}, {0x00248C, "ASUS"},
    {0x002618, "ASUS"}, {0x1C872C, "ASUS"}, {0x2C56DC, "ASUS"}, {0x305A3A, "ASUS"},
    {0x50465D, "ASUS"}, {0xAC220B, "ASUS"}, {0xF46D04, "ASUS"}, {0x00055D, "D-Link"},
    {0x000D88, "D-Link"}, {0x000F3D, "D-Link"}, {0x001195, "D-Link"}, {0x001346, "D-Link"},
    {0x0015E9, "D-Link"}, {0x00179A, "D-Link"}, {0x00195B, "D-Link"}, {0x001B11, "D-Link"},
    {0x001CF0, "D-Link"}, {0x001E58, "D-Link"}, {0x00A0C5, "Zyxel"}, {0x001349, "Zyxel"},
    {0x0019CB, "Zyxel"}, {0x001150, "Belkin"}, {0x001CDF, "Belkin"}, {0x08863B, "Belkin"},
    {0x944452, "Belkin"}, {0xEC1A59, "Belkin"}, {0x000BCD, "Buffalo"}, {0x001601, "Buffalo"},
    {0x001D73, "Buffalo"}, {0x0024A5, "Buffalo"}, {0x000C42, "MikroTik"}, {0x4C5E0C, "MikroTik"},
    {0x6C3B6B, "MikroTik"}, {0xD4CA6D, "MikroTik"}, {0xE48D8C, "MikroTik"}, {0x2CC81B, "MikroTik"},
    {0xCC2DE0, "MikroTik"}, {0x000B86, "Aruba"}, {0x001A1E, "Aruba"}, {0x24DEC6, "Aruba"},
    {0x6CF37F, "Aruba"}, {0x9C1C12, "Aruba"}, {0xD8C7C8, "Aruba"}, {0x2C5D93, "Ruckus"},
    {0x58B633, "Ruckus"}, {0x74911A, "Ruckus"}, {0xC4108A, "Ruckus"}, {0x5C5B35, "Juniper Mist"},
    {0x001B17, "Palo Alto Networks"}, {0x00090F, "Fortinet"}, {0x0000F0, "Samsung"}, {0x000DAE, "Samsung"},
    {0x001247, "Samsung"}, {0x0012FB, "Samsung"}, {0x001599, "Samsung"}, {0x0015B9, "Samsung"},
    {0x0016DB, "Samsung"}, {0x0017C9, "Samsung"}, {0x0017D5, "Samsung"}, {0x0018AF, "Samsung"},
    {0x001A8A, "Samsung"}, {0x1C62B8, "Samsung"}, {0x5CF6DC, "Samsung"}, {0x8C7712, "Samsung"},
    {0x640980, "Xiaomi"}, {0x34CE00, "Xiaomi"}, {0x286C07, "Xiaomi"}, {0x50EC50, "Xiaomi"},
    {0x64B473, "Xiaomi"}, {0x7802F8, "Xiaomi"}, {0xF8A45F, "Xiaomi"}, {0x3C5AB4, "Google"},
    {0x54609A, "Google"}, {0xF4F5D8, "Google"}, {0xF88FCA, "Google"}, {0x00BB3A, "Amazon"},
    {0x0C47C9, "Amazon"}, {0x44650D, "Amazon"}, {0x74C246, "Amazon"}, {0xF0272D, "Amazon"},
    {0x6837E9, "Amazon"}, {0x000E58, "Sonos"}, {0x5CAAFD, "Sonos"}, {0xB8E937, "Sonos"},
    {0x001302, "Intel"}, {0x001320, "Intel"}, {0x0013CE, "Intel"}, {0x0013E8, "Intel"},
    {0x001500, "Intel"}, {0x001B21, "Intel"}, {0x001E64, "Intel"}, {0x00E04C, "Realtek"},
    {0x000AEB, "Realtek"}, {0x001018, "Broadcom"}, {0x00037F, "Atheros"}, {0x001374, "Atheros"},
    {0x00C0CA, "Alfa Network"}, {0x000C43, "Ralink"}, {0x000CE7, "MediaTek"}, {0x240AC4, "Espressif"},
    {0x30AEA4, "Espressif"}, {0x246F28, "Espressif"}, {0x3C71BF, "Espressif"}, {0x5CCF7F, "Espressif"},
    {0x84CCA8, "Espressif"}, {0xA4CF12, "Espressif"}, {0xB4E62D, "Espressif"}, {0xC8C9A3, "Espressif"},
    {0xDC4F22, "Espressif"}, {0xB827EB, "Raspberry Pi"}, {0xDCA632, "Raspberry Pi"}, {0xE45F01, "Raspberry Pi"},
    {0x28CDC1, "Raspberry Pi"}, {0xD83ADD, "Raspberry Pi"}, {0x84F3EB, "Espressif"}, {0x10D561, "Tuya"},
    {0x7CF666, "Tuya"}, {0xD81F12, "Tuya"}, {0x8CAAB5, "Espressif"}, {0xE868E7, "Espressif"},
    {0x001C28, "Sagemcom"},
};
} // namespace

// The vendor of an OUI, "" when not in the table.
const char* ouiVendor(uint32_t oui) {
    for (const Oui& e : kOui) if (e.oui == oui) return e.name;
    return "";
}

} // namespace dect2::wifi
