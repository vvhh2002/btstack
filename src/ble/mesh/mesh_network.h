/*
 * Copyright (C) 2018 BlueKitchen GmbH
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

#ifndef __MESH_NETWORK
#define __MESH_NETWORK

#include "btstack_linked_list.h"
#include "provisioning.h"

#if defined __cplusplus
extern "C" {
#endif

typedef struct {
    btstack_linked_item_t item;
    uint8_t               len;
    uint8_t               data[29];
} mesh_network_pdu_t;

typedef struct {
    btstack_linked_item_t item;

    // index into shared global key list
    uint16_t netkey_index;

    // random net_key
    uint8_t net_key[16];

    // derivative data

    // k1
    uint8_t identity_key[16];
    uint8_t beacon_key[16];

    // k2
    uint8_t nid;
    uint8_t encryption_key[16];
    uint8_t privacy_key[16];

    // k3
    uint8_t network_id[8];

} mesh_network_key_t;

/**
 * @brief Init Mesh Network Layer
 */
void mesh_network_init(void);

/**
 * @brief Initialize network key list from provisioning data
 * @param provisioning_data
 */
void mesh_network_key_list_add_from_provisioning_data(const mesh_provisioning_data_t * provisioning_data);

/**
 * @brief Send TransportPDU after encryption
 * @param netkey_index
 * @param ctl
 * @param ttl
 * @param seq
 * @param dest
 * @param transport_pdu_data
 * @param transport_pdu_len
 */
uint8_t mesh_network_send(uint16_t netkey_index, uint8_t ctl, uint8_t ttl, uint32_t seq, uint16_t src, uint16_t dest,
                          const uint8_t * transport_pdu_data, uint8_t transport_pdu_len);

/**
 * @brief Validate network addresses
 * @param ctl
 * @param src
 * @param dst
 * @returns 1 if valid, 
 */
int mesh_network_addresses_valid(uint8_t ctl, uint16_t src, uint16_t dst);

// Testing only
void mesh_network_received_message(const uint8_t * pdu_data, uint8_t pdu_len);

#if defined __cplusplus
}
#endif

#endif
