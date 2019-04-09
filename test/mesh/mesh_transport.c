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

#define __BTSTACK_FILE__ "mesh_transport.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble/mesh/beacon.h"
#include "mesh_transport.h"
#include "btstack_util.h"
#include "btstack_memory.h"
#include "ble/mesh/mesh_lower_transport.h"
#include "mesh_peer.h"

static uint16_t primary_element_address;

static void mesh_print_hex(const char * name, const uint8_t * data, uint16_t len){
    printf("%-20s ", name);
    printf_hexdump(data, len);
}
// static void mesh_print_x(const char * name, uint32_t value){
//     printf("%20s: 0x%x", name, (int) value);
// }

// utility

// lower transport

// prototypes

// application key list

typedef struct {
    uint8_t  label_uuid[16];
    uint16_t pseudo_dst;
    uint16_t dst;
    uint8_t  akf;
    uint8_t  aid;
    uint8_t  first;
} mesh_transport_key_iterator_t;

typedef struct {
    btstack_linked_item_t item;

    // index into shared global key list
    uint16_t index;

    // app_key
    uint8_t key[16];

    // application key flag, 0 for device key
    uint8_t akf;

    // application key hash id
    uint8_t aid;
} mesh_transport_key_t;

typedef struct {
    uint16_t pseudo_dst;
    uint16_t hash;
    uint8_t  label_uuid[16];
} mesh_virtual_address_t;

static mesh_transport_key_t   test_application_key;
static mesh_transport_key_t   mesh_transport_device_key;
static mesh_virtual_address_t test_virtual_address;

// mesh network key iterator
static void mesh_transport_key_iterator_init(mesh_transport_key_iterator_t * it, uint16_t dst, uint8_t akf, uint8_t aid){
    it->dst = dst;
    it->aid      = aid;
    it->akf      = akf;
    it->first    = 1;
}

static int mesh_transport_key_iterator_has_more(mesh_transport_key_iterator_t * it){
    if (!it->first) return 0;
    if (mesh_network_address_virtual(it->dst) && it->dst != test_virtual_address.hash) return 0;
    if (it->akf){
        return it->aid == test_application_key.aid;
    } else {
        return 1;
    }
}

static const mesh_transport_key_t * mesh_transport_key_iterator_get_next(mesh_transport_key_iterator_t * it){
    it->first = 0;
    if (mesh_network_address_virtual(it->dst)){
        memcpy(it->label_uuid, test_virtual_address.label_uuid, 16);
        it->pseudo_dst = test_virtual_address.pseudo_dst;
    }
    if (it->akf){
        return &test_application_key;
    } else {
        return &mesh_transport_device_key;
    }
}

void mesh_application_key_set(uint16_t appkey_index, uint8_t aid, const uint8_t * application_key){
    test_application_key.index = appkey_index;
    test_application_key.aid   = aid;
    test_application_key.akf   = 1;
    memcpy(test_application_key.key, application_key, 16);
}

void mesh_transport_set_device_key(const uint8_t * device_key){
    mesh_transport_device_key.index = MESH_DEVICE_KEY_INDEX;
    mesh_transport_device_key.aid   = 0;
    mesh_transport_device_key.akf   = 0;
    memcpy(mesh_transport_device_key.key, device_key, 16);
}

static const mesh_transport_key_t * mesh_transport_key_get(uint16_t appkey_index){
    if (appkey_index == MESH_DEVICE_KEY_INDEX){
        return &mesh_transport_device_key;
    }
    if (appkey_index != test_application_key.index) return NULL;
    return &test_application_key;
}

static void mesh_virtual_address_run(void){
}

uint16_t mesh_virtual_address_register(uint8_t * label_uuid, uint16_t hash){
    // TODO:: check if already exists
    // TODO: calc hash
    test_virtual_address.hash   = hash;
    memcpy(test_virtual_address.label_uuid, label_uuid, 16);
    test_virtual_address.pseudo_dst = 0x8000;
    mesh_virtual_address_run();
    return test_virtual_address.pseudo_dst;
}

void mesh_virtual_address_unregister(uint16_t pseudo_dst){
}

static mesh_virtual_address_t * mesh_virtual_address_for_pseudo_dst(uint16_t pseudo_dst){
    if (test_virtual_address.pseudo_dst == pseudo_dst){
        return &test_virtual_address;
    }
    return NULL;
}

// UPPER TRANSPORT

// stub lower transport

static void mesh_upper_transport_validate_unsegmented_message(mesh_network_pdu_t * network_pdu);
static void mesh_upper_transport_validate_segmented_message(mesh_transport_pdu_t * transport_pdu);

static void mesh_transport_run(void);

static int crypto_active;
static mesh_network_pdu_t   * network_pdu_in_validation;
static mesh_transport_pdu_t * transport_pdu_in_validation;
static uint8_t application_nonce[13];
static btstack_crypto_ccm_t ccm;
static mesh_transport_key_iterator_t mesh_transport_key_it;

// upper transport callbacks - in access layer
static void (*mesh_access_unsegmented_handler)(mesh_network_pdu_t * network_pdu);
static void (*mesh_access_segmented_handler)(mesh_transport_pdu_t * transport_pdu);

// unsegmented (network) and segmented (transport) control and access messages
static btstack_linked_list_t upper_transport_incoming;


void mesh_upper_unsegmented_control_message_received(mesh_network_pdu_t * network_pdu){
    uint8_t * lower_transport_pdu     = mesh_network_pdu_data(network_pdu);
    uint8_t  opcode = lower_transport_pdu[0];
    if (mesh_access_unsegmented_handler){
        mesh_access_unsegmented_handler(network_pdu);
    } else {
        printf("[!] Unhandled Control message with opcode %02x\n", opcode);
    }
}

static void mesh_upper_transport_process_unsegmented_message_done(mesh_network_pdu_t *network_pdu){
    crypto_active = 0;
    mesh_lower_transport_message_processed_by_higher_layer((mesh_pdu_t *) network_pdu_in_validation);
    mesh_transport_run();
}

static void mesh_upper_transport_process_segmented_message_done(mesh_transport_pdu_t *transport_pdu){
    crypto_active = 0;
    mesh_lower_transport_message_processed_by_higher_layer((mesh_pdu_t *)transport_pdu);
    mesh_transport_run();
}

static uint32_t iv_index_for_ivi_nid(uint8_t ivi_nid){
    // get IV Index and IVI
    uint32_t iv_index = mesh_get_iv_index();
    int ivi = ivi_nid >> 7;

    // if least significant bit differs, use previous IV Index
    if ((iv_index & 1 ) ^ ivi){
        iv_index--;
    }
    return iv_index;
}

static void transport_unsegmented_setup_nonce(uint8_t * nonce, const mesh_network_pdu_t * network_pdu){
    nonce[1] = 0x00;    // SZMIC if a Segmented Access message or 0 for all other message formats
    memcpy(&nonce[2], &network_pdu->data[2], 7);
    big_endian_store_32(nonce, 9, iv_index_for_ivi_nid(network_pdu->data[0]));
}

static void transport_segmented_setup_nonce(uint8_t * nonce, const mesh_transport_pdu_t * transport_pdu){
    nonce[1] = transport_pdu->transmic_len == 8 ? 0x80 : 0x00;
    memcpy(&nonce[2], &transport_pdu->network_header[2], 7);
    big_endian_store_32(nonce, 9, iv_index_for_ivi_nid(transport_pdu->network_header[0]));
}

static void transport_unsegmented_setup_application_nonce(uint8_t * nonce, const mesh_network_pdu_t * network_pdu){
    nonce[0] = 0x01;
    transport_unsegmented_setup_nonce(nonce, network_pdu);
    mesh_print_hex("AppNonce", nonce, 13);
}

static void transport_unsegmented_setup_device_nonce(uint8_t * nonce, const mesh_network_pdu_t * network_pdu){
    nonce[0] = 0x02;
    transport_unsegmented_setup_nonce(nonce, network_pdu);
    mesh_print_hex("DeviceNonce", nonce, 13);
}

static void transport_segmented_setup_application_nonce(uint8_t * nonce, const mesh_transport_pdu_t * transport_pdu){
    nonce[0] = 0x01;
    transport_segmented_setup_nonce(nonce, transport_pdu);
    mesh_print_hex("AppNonce", nonce, 13);
}

static void transport_segmented_setup_device_nonce(uint8_t * nonce, const mesh_transport_pdu_t * transport_pdu){
    nonce[0] = 0x02;
    transport_segmented_setup_nonce(nonce, transport_pdu);
    mesh_print_hex("DeviceNonce", nonce, 13);
}

static void mesh_upper_transport_validate_unsegmented_message_ccm(void * arg){
    mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t *) arg;

    uint8_t * lower_transport_pdu     = mesh_network_pdu_data(network_pdu);
    uint8_t trans_mic_len = 4;

    // store TransMIC
    uint8_t trans_mic[8];
    btstack_crypto_ccm_get_authentication_value(&ccm, trans_mic);
    mesh_print_hex("TransMIC", trans_mic, trans_mic_len);

    uint8_t * upper_transport_pdu     = mesh_network_pdu_data(network_pdu) + 1;
    uint8_t   upper_transport_pdu_len = mesh_network_pdu_len(network_pdu)  - 1;

    mesh_print_hex("Decryted PDU", upper_transport_pdu, upper_transport_pdu_len - trans_mic_len);

    if (memcmp(trans_mic, &upper_transport_pdu[upper_transport_pdu_len - trans_mic_len], trans_mic_len) == 0){
        printf("TransMIC matches\n");

        // remove TransMIC from payload
        network_pdu->len -= trans_mic_len;

        // if virtual address, update dst to pseudo_dst
        if (mesh_network_address_virtual(mesh_network_dst(network_pdu))){
            big_endian_store_16(network_pdu->data, 7, mesh_transport_key_it.pseudo_dst);
        }

        // pass to upper layer
        if (mesh_access_unsegmented_handler){
            mesh_access_unsegmented_handler(network_pdu);
        } else {
            printf("[!] Unhandled Unsegmented Access message\n");
        }
        
        printf("\n");

        // done
        mesh_upper_transport_process_unsegmented_message_done(network_pdu);
    } else {
        uint8_t afk = lower_transport_pdu[0] & 0x40;
        if (afk){
            printf("TransMIC does not match, try next key\n");
            mesh_upper_transport_validate_unsegmented_message(network_pdu);
        } else {
            printf("TransMIC does not match device key, done\n");
            // done
            mesh_upper_transport_process_unsegmented_message_done(network_pdu);
        }
    }
}

static void mesh_upper_transport_validate_segmented_message_ccm(void * arg){
    mesh_transport_pdu_t * transport_pdu = (mesh_transport_pdu_t *) arg;

    uint8_t * upper_transport_pdu     =  transport_pdu->data;
    uint8_t   upper_transport_pdu_len =  transport_pdu->len - transport_pdu->transmic_len;
 
    mesh_print_hex("Decrypted PDU", upper_transport_pdu, upper_transport_pdu_len);

    // store TransMIC
    uint8_t trans_mic[8];
    btstack_crypto_ccm_get_authentication_value(&ccm, trans_mic);
    mesh_print_hex("TransMIC", trans_mic, transport_pdu->transmic_len);

    if (memcmp(trans_mic, &upper_transport_pdu[upper_transport_pdu_len], transport_pdu->transmic_len) == 0){
        printf("TransMIC matches\n");

        // remove TransMIC from payload
        transport_pdu->len -= transport_pdu->transmic_len;

        // if virtual address, update dst to pseudo_dst
        if (mesh_network_address_virtual(mesh_transport_dst(transport_pdu))){
            big_endian_store_16(transport_pdu->network_header, 7, mesh_transport_key_it.pseudo_dst);
        }

        // pass to upper layer
        if (mesh_access_segmented_handler){
            mesh_access_segmented_handler(transport_pdu);
        } else {
            printf("[!] Unhandled Segmented Access/Control message\n");
        }
        
        printf("\n");

        // done
        mesh_upper_transport_process_segmented_message_done(transport_pdu);
    } else {
        uint8_t akf = transport_pdu->akf_aid & 0x40;
        if (akf){
            printf("TransMIC does not match, try next key\n");
            mesh_upper_transport_validate_segmented_message(transport_pdu);
        } else {
            printf("TransMIC does not match device key, done\n");
            // done
            mesh_upper_transport_process_segmented_message_done(transport_pdu);
        }
    }
}

static void mesh_upper_transport_validate_segmented_message_digest(void * arg){
    mesh_transport_pdu_t * transport_pdu   = (mesh_transport_pdu_t*) arg;
    uint8_t   upper_transport_pdu_len      = transport_pdu_in_validation->len - transport_pdu_in_validation->transmic_len;
    uint8_t * upper_transport_pdu_data_in  = transport_pdu_in_validation->data;
    uint8_t * upper_transport_pdu_data_out = transport_pdu->data;
    btstack_crypto_ccm_decrypt_block(&ccm, upper_transport_pdu_len, upper_transport_pdu_data_in, upper_transport_pdu_data_out, &mesh_upper_transport_validate_segmented_message_ccm, transport_pdu);
}

static void mesh_upper_transport_validate_unsegmented_message_digest(void * arg){
    mesh_network_pdu_t * network_pdu       = (mesh_network_pdu_t *) arg;
    uint8_t   trans_mic_len = 4;
    uint8_t   lower_transport_pdu_len      = network_pdu_in_validation->len - 9;
    uint8_t * upper_transport_pdu_data_in  = &network_pdu_in_validation->data[10];
    uint8_t * upper_transport_pdu_data_out = &network_pdu->data[10];
    uint8_t   upper_transport_pdu_len      = lower_transport_pdu_len - 1 - trans_mic_len;
    btstack_crypto_ccm_decrypt_block(&ccm, upper_transport_pdu_len, upper_transport_pdu_data_in, upper_transport_pdu_data_out, &mesh_upper_transport_validate_unsegmented_message_ccm, network_pdu);
}

static void mesh_upper_transport_validate_unsegmented_message(mesh_network_pdu_t * network_pdu){

    if (!mesh_transport_key_iterator_has_more(&mesh_transport_key_it)){
        printf("No valid transport key found\n");
        mesh_upper_transport_process_unsegmented_message_done(network_pdu);
        return;
    }
    const mesh_transport_key_t * message_key = mesh_transport_key_iterator_get_next(&mesh_transport_key_it);

    if (message_key->akf){
        transport_unsegmented_setup_application_nonce(application_nonce, network_pdu_in_validation);
    } else {
        transport_unsegmented_setup_device_nonce(application_nonce, network_pdu_in_validation);
    }

    // store application / device key index
    mesh_print_hex("AppOrDevKey", message_key->key, 16);
    network_pdu->appkey_index = message_key->index; 

    // unsegmented message have TransMIC of 32 bit
    uint8_t trans_mic_len = 4;
    printf("Unsegmented Access message with TransMIC len 4\n");

    uint8_t   lower_transport_pdu_len = network_pdu_in_validation->len - 9;
    uint8_t * upper_transport_pdu_data = &network_pdu_in_validation->data[10];
    uint8_t   upper_transport_pdu_len  = lower_transport_pdu_len - 1 - trans_mic_len;

    mesh_print_hex("EncAccessPayload", upper_transport_pdu_data, upper_transport_pdu_len);

    // decrypt ccm
    crypto_active = 1;
    uint16_t aad_len  = 0;
    if (mesh_network_address_virtual(mesh_network_dst(network_pdu))){
        aad_len  = 16;
    }
    btstack_crypto_ccm_init(&ccm, message_key->key, application_nonce, upper_transport_pdu_len, aad_len, trans_mic_len);
    if (aad_len){
        btstack_crypto_ccm_digest(&ccm, mesh_transport_key_it.label_uuid, aad_len, &mesh_upper_transport_validate_unsegmented_message_digest, network_pdu);
    } else {
        mesh_upper_transport_validate_unsegmented_message_digest(network_pdu);
    }
}

static void mesh_upper_transport_validate_segmented_message(mesh_transport_pdu_t * transport_pdu){
    uint8_t * upper_transport_pdu_data =  transport_pdu->data;
    uint8_t   upper_transport_pdu_len  =  transport_pdu->len - transport_pdu->transmic_len;

    if (!mesh_transport_key_iterator_has_more(&mesh_transport_key_it)){
        printf("No valid transport key found\n");
        mesh_upper_transport_process_segmented_message_done(transport_pdu);
        return;
    }
    const mesh_transport_key_t * message_key = mesh_transport_key_iterator_get_next(&mesh_transport_key_it);

    if (message_key->akf){
        transport_segmented_setup_application_nonce(application_nonce, transport_pdu_in_validation);
    } else {
        transport_segmented_setup_device_nonce(application_nonce, transport_pdu_in_validation);
    }

    // store application / device key index
    mesh_print_hex("AppOrDevKey", message_key->key, 16);
    transport_pdu->appkey_index = message_key->index; 

    mesh_print_hex("EncAccessPayload", upper_transport_pdu_data, upper_transport_pdu_len);

    // decrypt ccm
    crypto_active = 1;
    uint16_t aad_len  = 0;
    if (mesh_network_address_virtual(mesh_transport_dst(transport_pdu))){
        aad_len  = 16;
    }
    btstack_crypto_ccm_init(&ccm, message_key->key, application_nonce, upper_transport_pdu_len, aad_len, transport_pdu->transmic_len);

    if (aad_len){
        btstack_crypto_ccm_digest(&ccm, mesh_transport_key_it.label_uuid, aad_len, &mesh_upper_transport_validate_segmented_message_digest, transport_pdu);
    } else {
        mesh_upper_transport_validate_segmented_message_digest(transport_pdu);
    }
}

static void mesh_upper_transport_process_unsegmented_access_message(mesh_network_pdu_t *network_pdu){
    // copy original pdu
    network_pdu->len = network_pdu_in_validation->len;
    memcpy(network_pdu->data, network_pdu_in_validation->data, network_pdu->len);

    // 
    uint8_t * lower_transport_pdu     = &network_pdu_in_validation->data[9];
    uint8_t   lower_transport_pdu_len = network_pdu_in_validation->len - 9;

    mesh_print_hex("Lower Transport network pdu", &network_pdu_in_validation->data[9], lower_transport_pdu_len);

    uint8_t aid =  lower_transport_pdu[0] & 0x3f;
    uint8_t akf = (lower_transport_pdu[0] & 0x40) >> 6;
    printf("AKF: %u\n",   akf);
    printf("AID: %02x\n", aid);

    mesh_transport_key_iterator_init(&mesh_transport_key_it, mesh_network_dst(network_pdu), akf, aid);
    mesh_upper_transport_validate_unsegmented_message(network_pdu);
}

static void mesh_upper_transport_process_message(mesh_transport_pdu_t * transport_pdu){
    // copy original pdu
    transport_pdu->len = transport_pdu_in_validation->len;
    memcpy(transport_pdu, transport_pdu_in_validation, sizeof(mesh_transport_pdu_t));

    // 
    uint8_t * upper_transport_pdu     =  transport_pdu->data;
    uint8_t   upper_transport_pdu_len =  transport_pdu->len - transport_pdu->transmic_len;
    mesh_print_hex("Upper Transport pdu", upper_transport_pdu, upper_transport_pdu_len);

    uint8_t aid =  transport_pdu->akf_aid & 0x3f;
    uint8_t akf = (transport_pdu->akf_aid & 0x40) >> 6;

    printf("AKF: %u\n",   akf);
    printf("AID: %02x\n", aid);

    mesh_transport_key_iterator_init(&mesh_transport_key_it, mesh_transport_dst(transport_pdu), akf, aid);
    mesh_upper_transport_validate_segmented_message(transport_pdu);
}

void mesh_upper_transport_segmented_message_received(mesh_transport_pdu_t *transport_pdu){
    btstack_linked_list_add_tail(&upper_transport_incoming, (btstack_linked_item_t*) transport_pdu);
    mesh_transport_run();
}

void mesh_upper_transport_unsegmented_message_received(mesh_network_pdu_t * network_pdu){
    btstack_linked_list_add_tail(&upper_transport_incoming, (btstack_linked_item_t*) network_pdu);
    mesh_transport_run();
}

static void mesh_upper_transport_send_unsegmented_access_pdu_ccm(void * arg){
    crypto_active = 0;

    mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t *) arg;
    uint8_t * upper_transport_pdu     = mesh_network_pdu_data(network_pdu) + 1;
    uint8_t   upper_transport_pdu_len = mesh_network_pdu_len(network_pdu)  - 1;
    mesh_print_hex("EncAccessPayload", upper_transport_pdu, upper_transport_pdu_len);
    // store TransMIC
    btstack_crypto_ccm_get_authentication_value(&ccm, &upper_transport_pdu[upper_transport_pdu_len]);
    mesh_print_hex("TransMIC", &upper_transport_pdu[upper_transport_pdu_len], 4);
    network_pdu->len += 4;
    // send network pdu
    mesh_network_send_pdu(network_pdu);
}

static void mesh_upper_transport_send_segmented_access_pdu_ccm(void * arg){
    crypto_active = 0;

    mesh_transport_pdu_t * transport_pdu = (mesh_transport_pdu_t *) arg;
    mesh_print_hex("EncAccessPayload", transport_pdu->data, transport_pdu->len);
    // store TransMIC
    btstack_crypto_ccm_get_authentication_value(&ccm, &transport_pdu->data[transport_pdu->len]);
    mesh_print_hex("TransMIC", &transport_pdu->data[transport_pdu->len], transport_pdu->transmic_len);
    transport_pdu->len += transport_pdu->transmic_len;
    mesh_lower_transport_send_segmented_pdu(transport_pdu);
}

uint8_t mesh_upper_transport_setup_unsegmented_control_pdu(mesh_network_pdu_t * network_pdu, uint16_t netkey_index, uint8_t ttl, uint16_t src, uint16_t dest, uint8_t opcode,
                          const uint8_t * control_pdu_data, uint16_t control_pdu_len){

    printf("[+] Upper transport, setup unsegmented Control PDU (opcode %02x): \n", opcode);
    printf_hexdump(control_pdu_data, control_pdu_len);

    if (control_pdu_len > 11) return 1;

    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;

    uint8_t transport_pdu_data[12];
    transport_pdu_data[0] = opcode;
    memcpy(&transport_pdu_data[1], control_pdu_data, control_pdu_len);
    uint16_t transport_pdu_len = control_pdu_len + 1;

    mesh_print_hex("LowerTransportPDU", transport_pdu_data, transport_pdu_len);
    // setup network_pdu
    mesh_network_setup_pdu(network_pdu, netkey_index, network_key->nid, 1, ttl, mesh_lower_transport_next_seq(), src, dest, transport_pdu_data, transport_pdu_len);

    return 0;
}

uint8_t mesh_upper_transport_setup_segmented_control_pdu(mesh_transport_pdu_t * transport_pdu, uint16_t netkey_index, uint8_t ttl, uint16_t src, uint16_t dest, uint8_t opcode,
                          const uint8_t * control_pdu_data, uint16_t control_pdu_len){

    printf("[+] Upper transport, setup segmented Control PDU (opcode %02x): \n", opcode);
    printf_hexdump(control_pdu_data, control_pdu_len);

    if (control_pdu_len > 256) return 1;

    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;

    uint32_t seq = mesh_lower_transport_peek_seq();

    memcpy(transport_pdu->data, control_pdu_data, control_pdu_len);
    transport_pdu->len = control_pdu_len;
    transport_pdu->netkey_index = netkey_index;
    transport_pdu->akf_aid = opcode;
    transport_pdu->transmic_len = 0;    // no TransMIC for control
    mesh_transport_set_nid_ivi(transport_pdu, network_key->nid);
    mesh_transport_set_seq(transport_pdu, seq);
    mesh_transport_set_src(transport_pdu, src);
    mesh_transport_set_dest(transport_pdu, dest);
    mesh_transport_set_ctl_ttl(transport_pdu, 0x80 | ttl);

    return 0;
}

uint8_t mesh_upper_transport_setup_unsegmented_access_pdu(mesh_network_pdu_t * network_pdu, uint16_t netkey_index, uint16_t appkey_index, uint8_t ttl, uint16_t src, uint16_t dest,
                          const uint8_t * access_pdu_data, uint8_t access_pdu_len){

    uint32_t seq = mesh_lower_transport_peek_seq();

    printf("[+] Upper transport, setup unsegmented Access PDU - seq %06x\n", seq);
    mesh_print_hex("Access Payload", access_pdu_data, access_pdu_len);

    // get app or device key
    const mesh_transport_key_t * appkey;
    appkey = mesh_transport_key_get(appkey_index);
    if (appkey == NULL){
        printf("appkey_index %x unknown\n", appkey_index);
        return 1;
    }
    uint8_t akf_aid = (appkey->akf << 6) | appkey->aid;

    // lookup network by netkey_index
    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;

    uint8_t transport_pdu_data[16];
    transport_pdu_data[0] = akf_aid;
    memcpy(&transport_pdu_data[1], access_pdu_data, access_pdu_len);
    uint16_t transport_pdu_len = access_pdu_len + 1;

    // setup network_pdu
    mesh_network_setup_pdu(network_pdu, netkey_index, network_key->nid, 0, ttl, mesh_lower_transport_next_seq(), src, dest, transport_pdu_data, transport_pdu_len);
    network_pdu->appkey_index = appkey_index;
    return 0;
}

uint8_t mesh_upper_transport_setup_segmented_access_pdu(mesh_transport_pdu_t * transport_pdu, uint16_t netkey_index, uint16_t appkey_index, uint8_t ttl, uint16_t src, uint16_t dest,
                          uint8_t szmic, const uint8_t * access_pdu_data, uint8_t access_pdu_len){

    uint32_t seq = mesh_lower_transport_peek_seq();

    printf("[+] Upper transport, setup segmented Access PDU - seq %06x, szmic %u, iv_index %08x\n", seq, szmic, mesh_get_iv_index());
    mesh_print_hex("Access Payload", access_pdu_data, access_pdu_len);

    // get app or device key
    const mesh_transport_key_t * appkey;
    appkey = mesh_transport_key_get(appkey_index);
    if (appkey == NULL){
        printf("appkey_index %x unknown\n", appkey_index);
        return 1;
    }
    uint8_t akf_aid = (appkey->akf << 6) | appkey->aid;

    // lookup network by netkey_index
    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;

    const uint8_t trans_mic_len = szmic ? 8 : 4;


    // store in transport pdu
    memcpy(transport_pdu->data, access_pdu_data, access_pdu_len);
    transport_pdu->len = access_pdu_len;
    transport_pdu->transmic_len = trans_mic_len;
    transport_pdu->netkey_index = netkey_index;
    transport_pdu->appkey_index = appkey_index;
    transport_pdu->akf_aid      = akf_aid;
    mesh_transport_set_nid_ivi(transport_pdu, network_key->nid | ((mesh_get_iv_index() & 1) << 7));
    mesh_transport_set_seq(transport_pdu, seq);
    mesh_transport_set_src(transport_pdu, src);
    mesh_transport_set_dest(transport_pdu, dest);
    mesh_transport_set_ctl_ttl(transport_pdu, ttl);

    return 0;
}

void mesh_upper_transport_send_unsegmented_control_pdu(mesh_network_pdu_t * network_pdu){
    mesh_network_send_pdu(network_pdu);
}

void mesh_upper_transport_send_segmented_control_pdu(mesh_transport_pdu_t * transport_pdu){
    mesh_lower_transport_send_segmented_pdu(transport_pdu);
}

void mesh_upper_transport_send_unsegmented_access_pdu_digest(void * arg){
    mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t *) arg;
    uint8_t * access_pdu_data = mesh_network_pdu_data(network_pdu) + 1;
    uint16_t  access_pdu_len  = mesh_network_pdu_len(network_pdu)  - 1;
    btstack_crypto_ccm_encrypt_block(&ccm, access_pdu_len, access_pdu_data, access_pdu_data, &mesh_upper_transport_send_unsegmented_access_pdu_ccm, network_pdu);
}

void mesh_upper_transport_send_unsegmented_access_pdu(mesh_network_pdu_t * network_pdu){

    // if dst is virtual address, lookup label uuid and hash
    uint16_t aad_len = 0;
    mesh_virtual_address_t * virtual_address = NULL;
    uint16_t dst = mesh_network_dst(network_pdu);
    if (mesh_network_address_virtual(dst)){
        virtual_address = mesh_virtual_address_for_pseudo_dst(dst);
        if (!virtual_address){
            printf("No virtual address register for pseudo dst %4x\n", dst);
            btstack_memory_mesh_network_pdu_free(network_pdu);
            return;
        }
        aad_len = 16;
        big_endian_store_16(network_pdu->data, 7, virtual_address->hash);
    }

    // setup nonce
    uint16_t appkey_index = network_pdu->appkey_index;
    if (appkey_index == MESH_DEVICE_KEY_INDEX){
        transport_unsegmented_setup_device_nonce(application_nonce, network_pdu);
    } else {
        transport_unsegmented_setup_application_nonce(application_nonce, network_pdu);
    }

    // get app or device key
    const mesh_transport_key_t * appkey = mesh_transport_key_get(appkey_index);
    mesh_print_hex("AppOrDevKey", appkey->key, 16);

    // encrypt ccm
    uint8_t   trans_mic_len = 4;
    uint16_t  access_pdu_len  = mesh_network_pdu_len(network_pdu)  - 1;
    crypto_active = 1;

    btstack_crypto_ccm_init(&ccm, appkey->key, application_nonce, access_pdu_len, aad_len, trans_mic_len);
    if (virtual_address){
        mesh_print_hex("LabelUUID", virtual_address->label_uuid, 16);
        btstack_crypto_ccm_digest(&ccm, virtual_address->label_uuid, 16, &mesh_upper_transport_send_unsegmented_access_pdu_digest, network_pdu);
    } else {
        mesh_upper_transport_send_unsegmented_access_pdu_digest(network_pdu);    
    }
}

void mesh_upper_transport_send_segmented_access_pdu_digest(void *arg){
    mesh_transport_pdu_t * transport_pdu = (mesh_transport_pdu_t *) arg;
    uint16_t  access_pdu_len  = transport_pdu->len;
    uint8_t * access_pdu_data = transport_pdu->data;
    btstack_crypto_ccm_encrypt_block(&ccm, access_pdu_len,access_pdu_data, access_pdu_data, &mesh_upper_transport_send_segmented_access_pdu_ccm, transport_pdu);
}

void mesh_upper_transport_send_segmented_access_pdu(mesh_transport_pdu_t * transport_pdu){

    // if dst is virtual address, lookup label uuid and hash
    uint16_t aad_len = 0;
    mesh_virtual_address_t * virtual_address = NULL;
    uint16_t dst = mesh_transport_dst(transport_pdu);
    if (mesh_network_address_virtual(dst)){
        virtual_address = mesh_virtual_address_for_pseudo_dst(dst);
        if (!virtual_address){
            printf("No virtual address register for pseudo dst %4x\n", dst);
            btstack_memory_mesh_transport_pdu_free(transport_pdu);
            return;
        }
        // printf("Using hash %4x with LabelUUID: ", virtual_address->hash);
        // printf_hexdump(virtual_address->label_uuid, 16);
        aad_len = 16;
        big_endian_store_16(transport_pdu->network_header, 7, virtual_address->hash);
    }

    // setup nonce - uses dst, so after pseudo address translation
    uint16_t appkey_index = transport_pdu->appkey_index;
    if (appkey_index == MESH_DEVICE_KEY_INDEX){
        transport_segmented_setup_device_nonce(application_nonce, transport_pdu);
    } else {
        transport_segmented_setup_application_nonce(application_nonce, transport_pdu);
    }

    // get app or device key
    const mesh_transport_key_t * appkey = mesh_transport_key_get(appkey_index);
    mesh_print_hex("AppOrDevKey", appkey->key, 16);

    // encrypt ccm
    uint8_t   transmic_len    = transport_pdu->transmic_len;
    uint16_t  access_pdu_len  = transport_pdu->len;
    crypto_active = 1;
    btstack_crypto_ccm_init(&ccm, appkey->key, application_nonce, access_pdu_len, aad_len, transmic_len);
    if (virtual_address){
        mesh_print_hex("LabelUUID", virtual_address->label_uuid, 16);
        btstack_crypto_ccm_digest(&ccm, virtual_address->label_uuid, 16, &mesh_upper_transport_send_segmented_access_pdu_digest, transport_pdu);
    } else {
        mesh_upper_transport_send_segmented_access_pdu_digest(transport_pdu);
    }
}

void mesh_upper_transport_set_primary_element_address(uint16_t unicast_address){
    primary_element_address = unicast_address;
}

void mesh_upper_transport_register_unsegemented_message_handler(void (*callback)(mesh_network_pdu_t * network_pdu)){
    mesh_access_unsegmented_handler = callback;
}
void mesh_upper_transport_register_segemented_message_handler(void (*callback)(mesh_transport_pdu_t * transport_pdu)){
    mesh_access_segmented_handler = callback;
}

void mesh_transport_init(){
    mesh_lower_transport_init();
}

static void mesh_transport_run(void){
    while(!btstack_linked_list_empty(&upper_transport_incoming)){

        if (crypto_active) return;

        // peek at next message
        mesh_pdu_t * pdu =  (mesh_pdu_t *) btstack_linked_list_get_first_item(&upper_transport_incoming);
        mesh_transport_pdu_t * transport_pdu;
        mesh_network_pdu_t   * network_pdu;
        switch (pdu->pdu_type){
            case MESH_PDU_TYPE_NETWORK:
                network_pdu = (mesh_network_pdu_t *) pdu;
                // control?
                if (mesh_network_control(network_pdu)) {
                    (void) btstack_linked_list_pop(&upper_transport_incoming);
                    mesh_upper_unsegmented_control_message_received(network_pdu);
                    mesh_lower_transport_message_processed_by_higher_layer((mesh_pdu_t *) network_pdu);
                } else {
                    mesh_network_pdu_t * decode_pdu = mesh_network_pdu_get();
                    if (!decode_pdu) return;
                    // get encoded network pdu and start processing
                    network_pdu_in_validation = network_pdu;
                    (void) btstack_linked_list_pop(&upper_transport_incoming);
                    mesh_upper_transport_process_unsegmented_access_message(decode_pdu);
                }
                break;
            case MESH_PDU_TYPE_TRANSPORT:
                transport_pdu = (mesh_transport_pdu_t *) pdu;
                uint8_t ctl = mesh_transport_ctl(transport_pdu);
                if (ctl){
                    printf("Ignoring Segmented Control Message\n");
                    (void) btstack_linked_list_pop(&upper_transport_incoming);
                    mesh_lower_transport_message_processed_by_higher_layer((mesh_pdu_t *) transport_pdu);
                } else {
                    mesh_transport_pdu_t * decode_pdu = mesh_transport_pdu_get();
                    if (!decode_pdu) return;
                    // get encoded transport pdu and start processing
                    transport_pdu_in_validation = transport_pdu;
                    (void) btstack_linked_list_pop(&upper_transport_incoming);
                    mesh_upper_transport_process_message(decode_pdu);
                }
                break;
            default:
                break;
        }
    }
}

// buffer pool
mesh_transport_pdu_t * mesh_transport_pdu_get(void){
    mesh_transport_pdu_t * transport_pdu = btstack_memory_mesh_transport_pdu_get();
    if (transport_pdu) {
        memset(transport_pdu, 0, sizeof(mesh_transport_pdu_t));
        transport_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_TRANSPORT;
    }
    return transport_pdu;
}

void mesh_transport_pdu_free(mesh_transport_pdu_t * transport_pdu){
    btstack_memory_mesh_transport_pdu_free(transport_pdu);
}