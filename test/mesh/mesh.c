/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "mesh.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble/mesh/adv_bearer.h"
#include "ble/mesh/beacon.h"
#include "ble/mesh/mesh_crypto.h"
#include "ble/mesh/pb_adv.h"
#include "ble/mesh/pb_gatt.h"
#include "ble/gatt-service/mesh_provisioning_service_server.h"
#include "provisioning.h"
#include "provisioning_device.h"
#include "mesh_transport.h"
#include "btstack.h"
#include "btstack_tlv.h"

static void show_usage(void);

#define BEACON_TYPE_SECURE_NETWORK 1

const static uint8_t device_uuid[] = { 0x00, 0x1B, 0xDC, 0x08, 0x10, 0x21, 0x0B, 0x0E, 0x0A, 0x0C, 0x00, 0x0B, 0x0E, 0x0A, 0x0C, 0x00 };

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static uint8_t mesh_flags;

static uint16_t pb_transport_cid = MESH_PB_TRANSPORT_INVALID_CID;

// pin entry
static int ui_chars_for_pin; 
static uint8_t ui_pin[17];
static int ui_pin_offset;

static const btstack_tlv_t * btstack_tlv_singleton_impl;
static void *                btstack_tlv_singleton_context;

static uint8_t beacon_key[16];
static uint8_t network_id[8];
static uint16_t primary_element_address;

// static void mesh_print_hex(const char * name, const uint8_t * data, uint16_t len){
//     printf("%-20s ", name);
//     printf_hexdump(data, len);
// }
// static void mesh_print_x(const char * name, uint32_t value){
//     printf("%20s: 0x%x", name, (int) value);
// }

static void mesh_provisioning_dump(const mesh_provisioning_data_t * data){
    printf("UnicastAddr:   0x%02x\n", data->unicast_address);
    printf("NID:           0x%02x\n", data->nid);
    printf("IV Index:      0x%08x\n", data->iv_index);
    printf("NetworkID:     "); printf_hexdump(data->network_id, 8);
    printf("BeaconKey:     "); printf_hexdump(data->beacon_key, 16);
    printf("EncryptionKey: "); printf_hexdump(data->encryption_key, 16);
    printf("PrivacyKey:    "); printf_hexdump(data->privacy_key, 16);
    printf("DevKey:        "); printf_hexdump(data->device_key, 16);
}

static void mesh_setup_from_provisioning_data(const mesh_provisioning_data_t * provisioning_data){
    // add to network key list
    mesh_network_key_list_add_from_provisioning_data(provisioning_data);
    // set unicast address
    mesh_network_set_primary_element_address(provisioning_data->unicast_address);
    mesh_upper_transport_set_primary_element_address(provisioning_data->unicast_address);
    primary_element_address = provisioning_data->unicast_address;
    // set iv_index
    mesh_set_iv_index(provisioning_data->iv_index);
    // set device_key
    mesh_transport_set_device_key(provisioning_data->device_key);
    // copy beacon key and network id
    memcpy(beacon_key, provisioning_data->beacon_key, 16);
    memcpy(network_id, provisioning_data->network_id, 8);
    // for secure beacon
    mesh_flags = provisioning_data->flags;
    // dump data
    mesh_provisioning_dump(provisioning_data);
}

static void mesh_load_app_keys(void){
    uint8_t data[2+1+16];
    int app_key_len = btstack_tlv_singleton_impl->get_tag(btstack_tlv_singleton_context, 'APPK', (uint8_t *) &data, sizeof(data));
    if (app_key_len){
        uint16_t appkey_index = little_endian_read_16(data, 0);
        uint8_t  aid          = data[2];
        uint8_t * application_key = &data[3];
        mesh_application_key_set(appkey_index, aid, application_key); 
        printf("Load AppKey: AppKey Index 0x%06x, AID %02x: ", appkey_index, aid);
        printf_hexdump(application_key, 16);
    }  else {
        printf("No Appkey stored\n");
    }
}

void mesh_store_app_key(uint16_t appkey_index, uint8_t aid, const uint8_t * application_key){
    printf("Store AppKey: AppKey Index 0x%06x, AID %02x: ", appkey_index, aid);
    uint8_t data[2+1+16];
    little_endian_store_16(data, 0, appkey_index);
    data[2] = aid;
    memcpy(&data[3], application_key, 16);
    btstack_tlv_singleton_impl->store_tag(btstack_tlv_singleton_context, 'APPK', (uint8_t *) &data, sizeof(data));
}

// helper network layer, temp
static uint8_t mesh_network_send(uint16_t netkey_index, uint8_t ctl, uint8_t ttl, uint32_t seq, uint16_t src, uint16_t dest, const uint8_t * transport_pdu_data, uint8_t transport_pdu_len){

    // "3.4.5.2: The output filter of the interface connected to advertising or GATT bearers shall drop all messages with TTL value set to 1."
    // if (ttl <= 1) return 0;

    // TODO: check transport_pdu_len depending on ctl

    // lookup network by netkey_index
    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 0;

    // allocate network_pdu
    mesh_network_pdu_t * network_pdu = mesh_network_pdu_get();
    if (!network_pdu) return 0;

    // setup network_pdu
    mesh_network_setup_pdu(network_pdu, netkey_index, network_key->nid, ctl, ttl, seq, src, dest, transport_pdu_data, transport_pdu_len);

    // send network_pdu
    mesh_network_send_pdu(network_pdu);
    return 0;
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    bd_addr_t addr;
    int i;
    int prov_len;
    mesh_provisioning_data_t provisioning_data;

    switch (packet_type) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
                    // dump bd_addr in pts format
                    gap_local_bd_addr(addr);
                    printf("Local addr: %s - ", bd_addr_to_str(addr));
                    for (i=0;i<6;i++) {
                        printf("%02x", addr[i]);
                    }
                    printf("\n");
                    // get tlv
                    btstack_tlv_get_instance(&btstack_tlv_singleton_impl, &btstack_tlv_singleton_context);
                    // load provisioning data
                    prov_len = btstack_tlv_singleton_impl->get_tag(btstack_tlv_singleton_context, 'PROV', (uint8_t *) &provisioning_data, sizeof(mesh_provisioning_data_t));
                    printf("Provisioning data available: %u\n", prov_len ? 1 : 0);
                    if (prov_len){
                        mesh_setup_from_provisioning_data(&provisioning_data);
                    } else {
                        printf("Starting Unprovisioned Device Beacon\n");
                        beacon_unprovisioned_device_start(device_uuid, 0);
                    }
                    // load app keys
                    mesh_load_app_keys();
                    // setup scanning
                    gap_set_scan_parameters(0, 0x300, 0x300);
                    gap_start_scan();
                    //
                    show_usage();
                    break;

                default:
                    break;
            }
            break;
    }
}

static void mesh_message_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    mesh_provisioning_data_t provisioning_data;
    switch(packet[0]){
        case HCI_EVENT_MESH_META:
            switch(packet[2]){
                case MESH_PB_TRANSPORT_LINK_OPEN:
                    printf("Provisioner link opened");
                    pb_transport_cid = mesh_pb_transport_link_open_event_get_pb_transport_cid(packet);
                    break;
                case MESH_PB_TRANSPORT_LINK_CLOSED:
                    pb_transport_cid = MESH_PB_TRANSPORT_INVALID_CID;
                    break;
                case MESH_PB_PROV_ATTENTION_TIMER:
                    printf("Attention Timer: %u\n", packet[3]);
                    break;
                case MESH_PB_PROV_INPUT_OOB_REQUEST:
                    printf("Enter passphrase: ");
                    fflush(stdout);
                    ui_chars_for_pin = 1;
                    ui_pin_offset = 0;
                    break;
                case MESH_PB_PROV_COMPLETE:
                    printf("Provisioning complete\n");
                    memcpy(provisioning_data.network_id, provisioning_device_data_get_network_id(), 8);
                    memcpy(provisioning_data.identity_key, provisioning_device_data_get_identity_key(), 16);
                    memcpy(provisioning_data.beacon_key, provisioning_device_data_get_beacon_key(), 16);
                    memcpy(provisioning_data.encryption_key, provisioning_device_data_get_encryption_key(), 16);
                    memcpy(provisioning_data.privacy_key, provisioning_device_data_get_privacy_key(), 16);
                    memcpy(provisioning_data.device_key, provisioning_device_data_get_device_key(), 16);
                    provisioning_data.iv_index = provisioning_device_data_get_iv_index();
                    provisioning_data.nid = provisioning_device_data_get_nid();
                    provisioning_data.flags = provisioning_device_data_get_flags();
                    provisioning_data.unicast_address = provisioning_device_data_get_unicast_address();
                    // store in TLV
                    btstack_tlv_singleton_impl->store_tag(btstack_tlv_singleton_context, 'PROV', (uint8_t *) &provisioning_data, sizeof(mesh_provisioning_data_t));
                    mesh_setup_from_provisioning_data(&provisioning_data);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void mesh_unprovisioned_beacon_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != MESH_BEACON_PACKET) return;
    uint8_t  device_uuid[16];
    uint16_t oob;
    memcpy(device_uuid, &packet[1], 16);
    oob = big_endian_read_16(packet, 17);
    printf("received unprovisioned device beacon, oob data %x, device uuid: ", oob);
    printf_hexdump(device_uuid, 16);
    pb_adv_create_link(device_uuid);
}

uint8_t      pts_device_uuid[16];
const char * pts_device_uuid_string = "001BDC0810210B0E0A0C000B0E0A0C00";

static int scan_hex_byte(const char * byte_string){
    int upper_nibble = nibble_for_char(*byte_string++);
    if (upper_nibble < 0) return -1;
    int lower_nibble = nibble_for_char(*byte_string);
    if (lower_nibble < 0) return -1;
    return (upper_nibble << 4) | lower_nibble;
}

static int btstack_parse_hex(const char * string, uint16_t len, uint8_t * buffer){
    int i;
    for (i = 0; i < len; i++) {
        int single_byte = scan_hex_byte(string);
        if (single_byte < 0) return 0;
        string += 2;
        buffer[i] = (uint8_t)single_byte;
        // don't check seperator after last byte
        if (i == len - 1) {
            return 1;
        }
        // optional seperator
        char separator = *string;
        if (separator == ':' && separator == '-' && separator == ' ') {
            string++;
        }
    }
    return 1;
}

static void btstack_print_hex(const uint8_t * data, uint16_t len, char separator){
    int i;
    for (i=0;i<len;i++){
        printf("%02x", data[i]);
        if (separator){
            printf("%c", separator);
        }
    }
    printf("\n");
}
static uint16_t pts_proxy_dst;
static int      pts_type;

static uint8_t      prov_static_oob_data[16];
static const char * prov_static_oob_string = "00000000000000000102030405060708";

static uint8_t      prov_public_key_data[64];
static const char * prov_public_key_string = "F465E43FF23D3F1B9DC7DFC04DA8758184DBC966204796ECCF0D6CF5E16500CC0201D048BCBBD899EEEFC424164E33C201C2B010CA6B4D43A8A155CAD8ECB279";
static uint8_t      prov_private_key_data[32];
static const char * prov_private_key_string = "529AA0670D72CD6497502ED473502B037E8803B5C60829A5A3CAA219505530BA";

static btstack_crypto_aes128_cmac_t mesh_cmac_request;
static uint8_t mesh_secure_network_beacon[22];
static uint8_t mesh_secure_network_beacon_auth_value[16];

static void load_pts_app_key(void){
    // PTS app key
    uint8_t application_key[16];
    const char * application_key_string = "3216D1509884B533248541792B877F98";
    btstack_parse_hex(application_key_string, 16, application_key);
    mesh_application_key_set(0, 0x38, application_key); 
    printf("PTS Application Key (AID %02x): ", 0x38);
    printf_hexdump(application_key, 16);
}

static void send_pts_network_messsage(int type){
    uint8_t lower_transport_pdu_data[16];

    uint16_t src = 0x0028;
    uint16_t dst = 0x0001;
    uint32_t seq = 0x00;
    uint8_t ttl = 0;
    uint8_t ctl = 0;

    switch (type){
        case 0:
            ttl = 0;
            dst = 0x001;
            printf("unicast ttl=0\n");
            break;
        case 1:
            dst = 0x001;
            ttl = 10;
            printf("unicast ttl=10\n");
            break;
        case 2:
            dst = 0x001;
            ttl = 0x7f;
            printf("unicast ttl=0x7f\n");
            break;
        case 3:
            printf("virtual\n");
            break;
        case 4:
            printf("group\n");
            break;
        case 5:
            printf("all-proxies\n");
            break;
        case 6:
            printf("all-friends\n");
            break;
        case 7:
            printf("all-relays\n");
            break;
        case 8:
            printf("all-nodes\n");
            break;
        default:
            return;
    }
    int lower_transport_pdu_len = 16;
    memset(lower_transport_pdu_data, 0x55, lower_transport_pdu_len);
    mesh_network_send(0, ctl, ttl, seq, src, dst, lower_transport_pdu_data, lower_transport_pdu_len);
}


static void send_pts_unsegmented_access_messsage(void){
    uint8_t access_pdu_data[16];

    load_pts_app_key();

    uint16_t src = primary_element_address;
    uint16_t dest = 0x0001;
    uint8_t  ttl = 10;

    int access_pdu_len = 1;
    memset(access_pdu_data, 0x55, access_pdu_len);
    uint16_t netkey_index = 0;
    uint16_t appkey_index = 0; // MESH_DEVICE_KEY_INDEX;

    // send as unsegmented access pdu
    mesh_network_pdu_t * network_pdu = mesh_network_pdu_get();
    int status = mesh_upper_transport_setup_unsegmented_access_pdu(network_pdu, netkey_index, appkey_index, ttl, src, dest, access_pdu_data, access_pdu_len);
    if (status) return;
    mesh_upper_transport_send_unsegmented_access_pdu(network_pdu);
}

static void send_pts_segmented_access_messsage_unicast(void){
    uint8_t access_pdu_data[20];

    load_pts_app_key();

    uint16_t src = primary_element_address;
    uint16_t dest = 0x0001;
    uint8_t  ttl = 10;

    int access_pdu_len = 20;
    memset(access_pdu_data, 0x55, access_pdu_len);
    uint16_t netkey_index = 0;
    uint16_t appkey_index = 0; // MESH_DEVICE_KEY_INDEX;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    int status = mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    if (status) return;
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}

static void send_pts_segmented_access_messsage_group(void){
    uint8_t access_pdu_data[20];

    load_pts_app_key();

    uint16_t src = primary_element_address;
    uint16_t dest = 0xd000;
    uint8_t  ttl = 10;

    int access_pdu_len = 20;
    memset(access_pdu_data, 0x55, access_pdu_len);
    uint16_t netkey_index = 0;
    uint16_t appkey_index = 0;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    int status = mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    if (status) return;
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}

static void send_pts_segmented_access_messsage_virtual(void){
    uint8_t access_pdu_data[20];

    load_pts_app_key();

    uint16_t src = primary_element_address;
    uint16_t dest = pts_proxy_dst;
    uint8_t  ttl = 10;

    int access_pdu_len = 20;
    memset(access_pdu_data, 0x55, access_pdu_len);
    uint16_t netkey_index = 0;
    uint16_t appkey_index = 0;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    int status = mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    if (status) return;
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}

static void mesh_secure_network_beacon_auth_value_calculated(void * arg){
    UNUSED(arg);
    memcpy(&mesh_secure_network_beacon[14], mesh_secure_network_beacon_auth_value, 8);
    printf("Secure Network Beacon\n");
    printf("- ");
    printf_hexdump(mesh_secure_network_beacon, sizeof(mesh_secure_network_beacon));
    adv_bearer_send_mesh_beacon(mesh_secure_network_beacon, sizeof(mesh_secure_network_beacon));
}

static void show_usage(void){
    bd_addr_t      iut_address;
    gap_local_bd_addr(iut_address);
    printf("\n--- Bluetooth Mesh Console at %s ---\n", bd_addr_to_str(iut_address));
    printf("1      - Send Unsegmented Access Message\n");
    printf("2      - Send   Segmented Access Message - Unicast\n");
    printf("3      - Send   Segmented Access Message - Group   D000\n");
    printf("4      - Send   Segmented Access Message - Virtual 9779\n");
    printf("6      - Clear Replay Protection List\n");
    printf("7      - Load PTS App key\n");
    printf("\n");
}

static void stdin_process(char cmd){
    if (ui_chars_for_pin){
        printf("%c", cmd);
        fflush(stdout);
        if (cmd == '\n'){
            printf("\nSending Pin '%s'\n", ui_pin);
            provisioning_device_input_oob_complete_alphanumeric(1, ui_pin, ui_pin_offset);
            ui_chars_for_pin = 0;
        } else {
            ui_pin[ui_pin_offset++] = cmd;
        }
        return;
    }
    switch (cmd){
        case '0':
            send_pts_network_messsage(pts_type++);
            break;
        case '1':
            send_pts_unsegmented_access_messsage();
            break;
        case '2':
            send_pts_segmented_access_messsage_unicast();
            break;
        case '3':
            send_pts_segmented_access_messsage_group();
            break;
        case '4':
            send_pts_segmented_access_messsage_virtual();
            break;
        case '6':
            printf("Clearing Replay Protection List\n");
            mesh_seq_auth_reset();
            break;
        case '7':
            load_pts_app_key();
            break;
        case '8':
            printf("Creating link to device uuid: ");
            printf_hexdump(pts_device_uuid, 16);
            pb_adv_create_link(pts_device_uuid);
            break;
        case '9':
            printf("Close link\n");
            pb_adv_close_link(1, 0);
            break;
        case 'p':
            printf("+ Public Key OOB Enabled\n");
            btstack_parse_hex(prov_public_key_string, 64, prov_public_key_data);
            btstack_parse_hex(prov_private_key_string, 32, prov_private_key_data);
            provisioning_device_set_public_key_oob(prov_public_key_data, prov_private_key_data);
            break;
        case 'o':
            printf("+ Output OOB Enabled\n");
            provisioning_device_set_output_oob_actions(0x08, 0x08);
            break;
        case 'i':
            printf("+ Input OOB Enabled\n");
            provisioning_device_set_input_oob_actions(0x08, 0x08);
            break;
        case 's':
            printf("+ Static OOB Enabled\n");
            btstack_parse_hex(prov_static_oob_string, 16, prov_static_oob_data);
            provisioning_device_set_static_oob(16, prov_static_oob_data);
            break;
        case 'b':
            printf("+ Setup Secure Network Beacon\n");
            mesh_secure_network_beacon[0] = BEACON_TYPE_SECURE_NETWORK;
            mesh_secure_network_beacon[1] = mesh_flags;
            memcpy(&mesh_secure_network_beacon[2], network_id, 8);
            big_endian_store_32(mesh_secure_network_beacon, 10, mesh_get_iv_index());
            btstack_crypto_aes128_cmac_message(&mesh_cmac_request, beacon_key, 13,
                &mesh_secure_network_beacon[1], mesh_secure_network_beacon_auth_value, &mesh_secure_network_beacon_auth_value_calculated, NULL);
            break;
        case ' ':
            show_usage();
            break;
        default:
            printf("Command: '%c' not implemented\n", cmd);
            show_usage();
            break;
    }
}

static void config_composition_data_status(void){

    printf("Received Config Composition Data Get -> send Config Composition Data Status\n");

    uint16_t src  = primary_element_address;
    uint16_t dest = 0x0001;
    uint8_t  ttl  = 10;

    uint16_t netkey_index = 0;
    uint16_t appkey_index = MESH_DEVICE_KEY_INDEX;

    uint8_t access_pdu_data[2 + 10 + 8];
    int access_pdu_len = sizeof(access_pdu_data);
    int pos = 0;
    access_pdu_data[pos++] = 0x02;
    access_pdu_data[pos++] = 0x00;

    // CID
    little_endian_store_16(access_pdu_data, pos, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH);
    pos += 2;
    // PID
    little_endian_store_16(access_pdu_data, pos, 0);
    pos += 2;
    // VID
    little_endian_store_16(access_pdu_data, pos, 0);
    pos += 2;
    // CRPL - number of protection list entries
    little_endian_store_16(access_pdu_data, pos, 1);
    pos += 2;
    // Features - Relay, Proxy, Friend, Lower Power, ...
    little_endian_store_16(access_pdu_data, pos, 0);
    pos += 2;

    // Element 1
    // Loc - bottom - https://www.bluetooth.com/specifications/assigned-numbers/gatt-namespace-descriptors
    little_endian_store_16(access_pdu_data, pos, 0x0103);
    pos += 2;
    // NumS - Configuration Server + Health Server
    access_pdu_data[pos++] = 2;
    // NumV
    access_pdu_data[pos++] = 0;
    // SIG Model: Configuration Server 0x0000
    little_endian_store_16(access_pdu_data, pos, 0);
    pos += 2;
    // SIG Model: Health Server 0x0002
    little_endian_store_16(access_pdu_data, pos, 0x0002);
    pos += 2;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}

static void config_appkey_status(uint32_t netkey_and_appkey_index, uint8_t status){
    uint16_t src  = primary_element_address;
    uint16_t dest = 0x0001;
    uint8_t  ttl  = 10;

    uint16_t netkey_index = 0;
    uint16_t appkey_index = MESH_DEVICE_KEY_INDEX;

    uint8_t access_pdu_data[2 + 4];
    int access_pdu_len = sizeof(access_pdu_data);
    int pos = 0;
    access_pdu_data[pos++] = 0x80;
    access_pdu_data[pos++] = 0x03;
    access_pdu_data[pos++] = status;
    little_endian_store_24(access_pdu_data, pos, netkey_and_appkey_index);
    pos += 3;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}


static uint32_t netkey_and_appkey_index;
static uint8_t  new_app_key[16];
static uint8_t  new_aid;
static uint16_t new_netkey_index;
static uint16_t new_appkey_index;

static void config_appkey_add_aid(void * arg){
    UNUSED(arg);
    printf("Config Appkey Add: NetKey Index 0x%06x, AppKey Index 0x%06x, AID %02x: ", new_netkey_index, new_appkey_index, new_aid);
    printf_hexdump(new_app_key, 16);

    // store in TLV
    mesh_store_app_key(new_appkey_index, new_aid, new_app_key);

    // set as main app key
    mesh_application_key_set(new_appkey_index, new_aid, new_app_key);

    config_appkey_status(netkey_and_appkey_index, 0);
}

static void config_appkey_add_handler(mesh_transport_pdu_t * transport_pdu){
    // 00: opcode 00
    // 01-03: netkey and appkey index
    netkey_and_appkey_index = little_endian_read_24(transport_pdu->data, 1);
    new_netkey_index = netkey_and_appkey_index & 0xfff;
    new_appkey_index = netkey_and_appkey_index >> 12;
    reverse_128(&transport_pdu->data[4], new_app_key);

    // calculate AID
    mesh_k4(&mesh_cmac_request, new_app_key, &new_aid, config_appkey_add_aid, NULL);
}

static void config_model_subscription_status(uint8_t status, uint16_t element_address, uint16_t address, uint32_t model_identifier){
    uint16_t src  = primary_element_address;
    uint16_t dest = 0x0001;
    uint8_t  ttl  = 10;

    uint16_t netkey_index = 0;
    uint16_t appkey_index = MESH_DEVICE_KEY_INDEX;

    uint8_t access_pdu_data[2 + 7];
    int access_pdu_len = sizeof(access_pdu_data);
    int pos = 0;
    access_pdu_data[pos++] = 0x80;
    access_pdu_data[pos++] = 0x1F;
    access_pdu_data[pos++] = status;
    little_endian_store_16(access_pdu_data, pos, element_address);
    pos += 2;
    little_endian_store_16(access_pdu_data, pos, address);
    pos += 2;
    little_endian_store_16(access_pdu_data, pos, model_identifier);
    pos += 2;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}

static void config_model_subscription_add_handler(mesh_transport_pdu_t * transport_pdu){
    uint16_t element_address = little_endian_read_16(transport_pdu->data, 2);
    uint16_t address = little_endian_read_16(transport_pdu->data, 4);
    uint16_t model_identifier = little_endian_read_16(transport_pdu->data, 6);

    config_model_subscription_status(0, element_address, address, model_identifier);    
}
static void config_model_subscription_virtual_address_add_handler(mesh_transport_pdu_t * transport_pdu){
    config_model_subscription_add_handler(transport_pdu);
}

static void config_model_app_status(uint8_t status, uint16_t element_address, uint16_t app_key_index, uint32_t model_identifier){
    uint16_t src  = primary_element_address;
    uint16_t dest = 0x0001;
    uint8_t  ttl  = 10;

    uint16_t netkey_index = 0;
    uint16_t appkey_index = MESH_DEVICE_KEY_INDEX;

    uint8_t access_pdu_data[2 + 7];
    int access_pdu_len = sizeof(access_pdu_data);
    int pos = 0;
    access_pdu_data[pos++] = 0x80;
    access_pdu_data[pos++] = 0x3E;
    access_pdu_data[pos++] = status;
    little_endian_store_16(access_pdu_data, pos, element_address);
    pos += 2;
    little_endian_store_16(access_pdu_data, pos, app_key_index);
    pos += 2;
    little_endian_store_16(access_pdu_data, pos, model_identifier);
    pos += 2;

    // send as segmented access pdu
    mesh_transport_pdu_t * transport_pdu = mesh_transport_pdu_get();
    mesh_upper_transport_setup_segmented_access_pdu(transport_pdu, netkey_index, appkey_index, ttl, src, dest, 0, access_pdu_data, access_pdu_len);
    mesh_upper_transport_send_segmented_access_pdu(transport_pdu);
}

static void config_model_app_bind_handler(mesh_transport_pdu_t * transport_pdu){
    uint16_t element_address = little_endian_read_16(transport_pdu->data, 2);
    uint16_t app_key_index = little_endian_read_16(transport_pdu->data, 4);
    uint16_t model_identifier = little_endian_read_16(transport_pdu->data, 6);

    config_model_app_status(0, element_address, app_key_index, model_identifier);
}

static const uint8_t config_appkey_add[] =                             { 0x00 };
static const uint8_t config_composition_data_get[] =                   { 0x80, 0x08, 0xff };
static const uint8_t config_model_subscription_add[] =                 { 0x80, 0x1b };
static const uint8_t config_model_subscription_virtual_address_add[] = { 0x80, 0x20 };
static const uint8_t config_model_app_bind[] =                         { 0x80, 0x3d };

void mesh_segemented_message_handler(mesh_transport_pdu_t * transport_pdu){
    printf("MESH Access Message: ");
    printf_hexdump(transport_pdu->data, transport_pdu->len);

    if ( (transport_pdu->len == sizeof(config_composition_data_get)) && memcmp(transport_pdu->data, config_composition_data_get, sizeof(config_composition_data_get)) == 0){
        printf("MESH config_composition_data_get\n");
        config_composition_data_status();
    }
    if ( (transport_pdu->len > sizeof(config_appkey_add)) && memcmp(transport_pdu->data, config_appkey_add, sizeof(config_appkey_add)) == 0){
        printf("MESH config_appkey_add\n");
        config_appkey_add_handler(transport_pdu);
    }
    if ( (transport_pdu->len > sizeof(config_model_subscription_add)) && memcmp(transport_pdu->data, config_model_subscription_add, sizeof(config_model_subscription_add)) == 0){
        printf("MESH config_model_subscription_add\n");
        config_model_subscription_add_handler(transport_pdu);
    }
    if ( (transport_pdu->len > sizeof(config_model_subscription_virtual_address_add)) && memcmp(transport_pdu->data, config_model_subscription_virtual_address_add, sizeof(config_model_subscription_virtual_address_add)) == 0){
        printf("MESH config_model_subscription_virtual_address_add_handler\n");
        config_model_subscription_virtual_address_add_handler(transport_pdu);
    }
    if ( (transport_pdu->len > sizeof(config_model_app_bind)) && memcmp(transport_pdu->data, config_model_app_bind, sizeof(config_model_app_bind)) == 0){
        printf("MESH config_model_app_bind\n");
        config_model_app_bind_handler(transport_pdu);
    }
}

int btstack_main(void);
int btstack_main(void)
{
    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // console
    btstack_stdin_setup(stdin_process);

    // crypto
    btstack_crypto_init();

    // 
    sm_init();

    // mesh
    adv_bearer_init();

    beacon_init();
    beacon_register_for_unprovisioned_device_beacons(&mesh_unprovisioned_beacon_handler);
    
    // Provisioning in device role
    provisioning_device_init(device_uuid);
    provisioning_device_register_packet_handler(&mesh_message_handler);

    // Network layer
    mesh_network_init();

    // Transport layers (lower + upper))
    mesh_transport_init();
    mesh_upper_transport_register_segemented_message_handler(&mesh_segemented_message_handler);

    // PTS Virtual Address Label UUID - without Config Model, PTS uses our device uuid
    uint8_t label_uuid[16];
    btstack_parse_hex("001BDC0810210B0E0A0C000B0E0A0C00", 16, label_uuid);
    pts_proxy_dst = mesh_virtual_address_register(label_uuid, 0x9779);


    //
    btstack_parse_hex(pts_device_uuid_string, 16, pts_device_uuid);
    btstack_print_hex(pts_device_uuid, 16, 0);

    // turn on!
	hci_power_control(HCI_POWER_ON);
	    
    return 0;
}
/* EXAMPLE_END */