/*****************************************************************************
 *   Ledger App Bitcoin.
 *   (c) 2021 Ledger SAS.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

#include <stdint.h>

#include "os.h"
#include "cx.h"

#include "../boilerplate/dispatcher.h"
#include "../boilerplate/sw.h"
#include "../common/merkle.h"
#include "../common/psbt.h"
#include "../common/read.h"
#include "../common/write.h"

#include "../constants.h"
#include "../types.h"
#include "../crypto.h"
#include "../ui/display.h"
#include "../ui/menu.h"

#include "client_commands.h"

#include "sign_psbt.h"


static void process_next_input(dispatcher_context_t *dc);
static void receive_global_tx_info(dispatcher_context_t *dc);
static void request_next_input_map(dispatcher_context_t *dc);
static void process_input_map(dispatcher_context_t *dc);
static void receive_sighash_type(dispatcher_context_t *dc);
static void request_non_witness_utxo(dispatcher_context_t *dc);
static void receive_non_witness_utxo(dispatcher_context_t *dc);

static void sign_legacy(dispatcher_context_t *dc);
static void sign_legacy_first_pass_completed(dispatcher_context_t *dc);

static void sign_legacy_validate_redeemScript(dispatcher_context_t *dc);

static void sign_legacy_start_second_pass(dispatcher_context_t *dc);

static void compute_sighash_and_sign_legacy(dispatcher_context_t *dc);

static void sign_segwit(dispatcher_context_t *dc);


/**
 * Validates the input, initializes the hash context and starts accumulating the wallet header in it.
 */
void handler_sign_psbt(
    uint8_t p1,
    uint8_t p2,
    uint8_t lc,
    dispatcher_context_t *dc
) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    if (p1 != 0 || p2 != 0) {
        dc->send_sw(SW_WRONG_P1P2);
        return;
    }

    // Device must be unlocked
    if (os_global_pin_is_validated() != BOLOS_UX_OK) {
        dc->send_sw(SW_SECURITY_STATUS_NOT_SATISFIED);
        return;
    }

    if (!buffer_read_varint(&dc->read_buffer, &state->global_map.size)) {
        dc->send_sw(SW_WRONG_DATA_LENGTH);
        return;
    }
    if (state->global_map.size > 252) {
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }


    if (!buffer_read_bytes(&dc->read_buffer, state->global_map.keys_root, 20)
        || !buffer_read_bytes(&dc->read_buffer, state->global_map.values_root, 20))
    {
        LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);
        dc->send_sw(SW_WRONG_DATA_LENGTH);
        return;
    }


    uint64_t n_inputs;
    if (!buffer_read_varint(&dc->read_buffer, &n_inputs)
        || !buffer_read_bytes(&dc->read_buffer, state->inputs_root, 20))
    {
        dc->send_sw(SW_WRONG_DATA_LENGTH);
        return;
    }
    if (n_inputs > 252) {
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }
    state->n_inputs = (size_t)n_inputs;


    uint64_t n_outputs;
    if (!buffer_read_varint(&dc->read_buffer, &n_outputs)
        || !buffer_read_bytes(&dc->read_buffer, state->outputs_root, 20))
    {
        dc->send_sw(SW_WRONG_DATA_LENGTH);
        return;
    }
    if (n_outputs > 252) {
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }
    state->n_outputs = (size_t)n_outputs;


    // Get the master's key fingerprint
    uint8_t master_pub_key[33];
    uint32_t bip32_path[] = {};
    crypto_get_compressed_pubkey_at_path(bip32_path, 0, master_pub_key, NULL);
    state->master_key_fingerprint = crypto_get_key_fingerprint(master_pub_key);


    // Check integrity of the global map
    call_check_merkle_tree_sorted(dc,
                                  &state->subcontext.check_merkle_tree_sorted,
                                  process_next_input,
                                  state->global_map.keys_root,
                                  (size_t)state->global_map.size);
}


static void process_next_input(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    cx_sha256_init(&state->hash_context);

    state->tmp[0] = PSBT_GLOBAL_UNSIGNED_TX;
    call_psbt_parse_rawtx(dc,
                          &state->subcontext.psbt_parse_rawtx,
                          receive_global_tx_info,
                          &state->hash_context,
                          &state->global_map,
                          state->tmp,
                          1,
                          PARSEMODE_TXID,
                          state->cur_input_index,
                          -1, // output index, not used
                          0  // ignored
                          );
}

static void receive_global_tx_info(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    // Keep track of the input's prevout hash and index
    memcpy(state->cur_prevout_hash, state->subcontext.psbt_parse_rawtx.program_state.compute_txid.prevout_hash, 32);
    state->cur_prevout_n = state->subcontext.psbt_parse_rawtx.program_state.compute_txid.prevout_n;

    // TODO: remove debug info
    PRINTF("Prevout hash for input %d: ", state->cur_input_index);
    for (int i = 0; i < 32; i++) PRINTF("%02x", state->cur_prevout_hash[i]);
    PRINTF("\n");

    if (state->n_inputs != state->subcontext.psbt_parse_rawtx.n_inputs) {
        PRINTF("Mismatching n_inputs.");
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }

    if (state->n_outputs != state->subcontext.psbt_parse_rawtx.n_outputs) {
        PRINTF("Mismatching n_outputs.");
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }

    state->cur_input_index = 0;
    dc->next(request_next_input_map);
}


/**
 * Callback to process all the keys of the current input map.
 * Keeps track if the current input has a witness_utxo and/or a redeemScript.
 */
static void input_keys_callback(sign_psbt_state_t *state, buffer_t *data) {
    size_t data_len = data->size - data->offset;
    if (data_len >= 1) {
        uint8_t key_type;
        buffer_read_u8(data, &key_type);
        if (key_type == PSBT_IN_WITNESS_UTXO) {
            state->cur_input_has_witnessUtxo = true;
        } else if (key_type == PSBT_IN_REDEEM_SCRIPT) {
            state->cur_input_has_redeemScript = true;
        } else if (key_type == PSBT_IN_SIGHASH_TYPE) {
            state->cur_input_has_sighash_type = true;
        }
    }
}

static void request_next_input_map(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    state->cur_input_has_witnessUtxo = false;
    state->cur_input_has_redeemScript = false;
    call_get_merkleized_map_with_callback(dc,
                                          &state->subcontext.get_merkleized_map,
                                          process_input_map,
                                          state->inputs_root,
                                          state->n_inputs,
                                          state->cur_input_index,
                                          make_callback(state, (dispatcher_callback_t)input_keys_callback),
                                          &state->cur_input_map);
}

static void process_input_map(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    if (state->cur_input_has_sighash_type) {
        // Get sighash type
        state->tmp[0] = PSBT_IN_SIGHASH_TYPE;
        call_get_merkleized_map_value(dc, &state->subcontext.get_merkleized_map_value, receive_sighash_type,
                                      &state->cur_input_map,
                                      state->tmp,
                                      1,
                                      state->cur_input_sighash_type_le,
                                      sizeof(state->cur_input_sighash_type_le));
    } else {
        // PSBT_IN_SIGHASH_TYPE is compulsory
        PRINTF("Missing SIGHASH TYPE for input %d\n", state->cur_input_index);
        dc->send_sw(SW_INCORRECT_DATA);
    }
}


static void receive_sighash_type(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    state->cur_input_sighash_type = read_u32_le(state->cur_input_sighash_type_le, 0);
    dc->next(request_non_witness_utxo);
}


static void request_non_witness_utxo(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);
    state->tmp[0] = PSBT_IN_NON_WITNESS_UTXO;

    cx_sha256_init(&state->hash_context);

    call_psbt_parse_rawtx(dc,
                          &state->subcontext.psbt_parse_rawtx,
                          receive_non_witness_utxo,
                          &state->hash_context,
                          &state->cur_input_map,
                          state->tmp,
                          1,
                          PARSEMODE_TXID,
                          -1,
                          state->cur_prevout_n,
                          0 // IGNORED
                          );
}

static void receive_non_witness_utxo(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    uint8_t txhash[32];

    crypto_hash_digest(&state->hash_context.header, txhash, 32);
    cx_hash_sha256(txhash, 32, txhash, 32);

    if (memcmp(txhash, state->cur_prevout_hash, 32) != 0) {
        PRINTF("Prevout hash did not match non-witness-utxo transaction hash.\n");

        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }


    state->cur_input_prevout_scriptpubkey_len = state->subcontext.psbt_parse_rawtx.program_state.compute_txid.vout_scriptpubkey_len;

    if (state->cur_input_prevout_scriptpubkey_len > MAX_PREVOUT_SCRIPTPUBKEY_LEN) {
        PRINTF("Prevout's scriptPubKey too long: %d\n", state->cur_input_prevout_scriptpubkey_len);
        dc->send_sw(SW_SIGNATURE_FAIL);
        return;
    }

    memcpy(state->cur_input_prevout_scriptpubkey,
           state->subcontext.psbt_parse_rawtx.program_state.compute_txid.vout_scriptpubkey,
           state->cur_input_prevout_scriptpubkey_len);

    if (!state->cur_input_has_witnessUtxo) {
        dc->next(sign_legacy);
    } else {
        dc->next(sign_segwit);
    }
}



static void sign_legacy(dispatcher_context_t *dc) {
    // sign legacy P2PKH or P2SH

    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    // sign_non_witness(non_witness_utxo.vout[psbt.tx.input_[i].prevout.n].scriptPubKey, i)

    cx_sha256_init(&state->hash_context);

    state->tmp[0] = PSBT_GLOBAL_UNSIGNED_TX;
    call_psbt_parse_rawtx(dc,
                          &state->subcontext.psbt_parse_rawtx,
                          sign_legacy_first_pass_completed,
                          &state->hash_context,
                          &state->global_map,
                          state->tmp,
                          1,
                          PARSEMODE_LEGACY_PASS1,
                          state->cur_input_index,
                          -1, // output index, not used
                          state->cur_input_sighash_type
                          );
}



static void sign_legacy_first_pass_completed(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);


    if (!state->cur_input_has_redeemScript) {
        // P2PKH, the script_code is the prevout's scriptPubKey
        crypto_hash_update_varint(&state->hash_context.header, state->cur_input_prevout_scriptpubkey_len);
        crypto_hash_update(&state->hash_context.header,
                           state->cur_input_prevout_scriptpubkey,
                           state->cur_input_prevout_scriptpubkey_len);
        dc->next(sign_legacy_start_second_pass);
    } else {
        // P2SH, the script_code is the redeemScript
        state->tmp[0] = PSBT_IN_SIGHASH_TYPE;
        call_psbt_process_redeemScript(dc, &state->subcontext.psbt_process_redeemScript, sign_legacy_validate_redeemScript,
                                       &state->hash_context,
                                       &state->cur_input_map,
                                       state->tmp,
                                       1);
    }
}

static void sign_legacy_validate_redeemScript(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    // TODO: P2SH still untested.

    if (state->cur_input_prevout_scriptpubkey_len != 1 + 20 + 1) {
        PRINTF("P2SH's scriptPubKey should be exactly 22 bytes\n");
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }

    if (memcmp(state->cur_input_prevout_scriptpubkey, state->subcontext.psbt_process_redeemScript.p2sh_script, 22) != 0) {
        PRINTF("redeemScript does not match prevout's scriptPubKey\n");
        dc->send_sw(SW_INCORRECT_DATA);
        return;
    }

    dc->next(sign_legacy_start_second_pass);
}


static void sign_legacy_start_second_pass(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    state->tmp[0] = PSBT_GLOBAL_UNSIGNED_TX;
    call_psbt_parse_rawtx(dc,
                          &state->subcontext.psbt_parse_rawtx,
                          compute_sighash_and_sign_legacy,
                          &state->hash_context,
                          &state->global_map,
                          state->tmp,
                          1,
                          PARSEMODE_LEGACY_PASS2,
                          state->cur_input_index,
                          -1, // output index, not used
                          state->cur_input_sighash_type
                          );
}


static void compute_sighash_and_sign_legacy(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    uint8_t sighash[32];

    //compute sighash
    crypto_hash_digest(&state->hash_context.header, sighash, 32);
    cx_hash_sha256(sighash, 32, sighash, 32);

    // TODO: remove
    PRINTF("sighash: ");
    for (int i = 0; i < 32; i++)
        PRINTF("%02x", sighash[i]);
    PRINTF("\n");


    cx_ecfp_private_key_t private_key = {0};
    uint8_t chain_code[32] = {0};
    uint32_t info = 0;

    // TODO: should read this from the PSBT
    const uint32_t sign_path[] = {
        // m/44'/1'/0'/1/1
        44 ^ 0x80000000,
         1 ^ 0x80000000,
         0 ^ 0x80000000,
         1,
         1
    };


    // TODO: refactor the signing code elsewhere
    crypto_derive_private_key(&private_key, chain_code, sign_path, 5);

    uint8_t sig[MAX_DER_SIG_LEN];

    int sig_len = 0;
    BEGIN_TRY {
        TRY {
            sig_len = cx_ecdsa_sign(&private_key,
                                    CX_RND_RFC6979,
                                    CX_SHA256,
                                    sighash,
                                    32,
                                    sig,
                                    MAX_DER_SIG_LEN,
                                    &info);
        }
        CATCH_OTHER(e) {
            return;
        }
        FINALLY {
            explicit_bzero(&private_key, sizeof(private_key));
        }
    }
    END_TRY;

    // TODO: remove
    PRINTF("signature for input %d: ", state->cur_input_index);
    for (int i = 0; i < sig_len; i++)
        PRINTF("%02x", sig[i]);
    PRINTF("\n");

    dc->send_sw(SW_OK);
}


static void sign_segwit(dispatcher_context_t *dc) {
    sign_psbt_state_t *state = (sign_psbt_state_t *)&G_command_state;

    LOG_PROCESSOR(dc, __FILE__, __LINE__, __func__);

    PRINTF("NOT IMPLEMENTED\n");
    dc->send_sw(SW_BAD_STATE);
}
