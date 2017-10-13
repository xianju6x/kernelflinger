/*
 * Copyright (c) 2017, Intel Corporation
 * All rights reserved.
 *
 * Author: genshen <genshen.li@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <lib.h>
#include <openssl/engine.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/hkdf.h>
#include <openssl/mem.h>
#include <openssl/sha.h>
#include "protocol/Mmc.h"
#include "protocol/SdHostIo.h"
#include "sdio.h"
#include "storage.h"
#include "rpmb.h"
#include "rpmb_storage.h"

#define RPMB_DEVICE_STATE_BLOCK_COUNT            1
#define RPMB_DEVICE_STATE_BLOCK_ADDR             2
#define RPMB_BLOCK_SIZE                          256
#define RPMB_ROLLBACK_INDEX_COUNT_PER_BLOCK      (RPMB_BLOCK_SIZE/8)
#define RPMB_ROLLBACK_INDEX_BLOCK_TOTAL_COUNT    8
#define RPMB_ROLLBACK_INDEX_BLOCK_ADDR           3
#define DEVICE_STATE_MAGIC 0xDC

static rpmb_storage_t rpmb_ops;
static UINT8 rpmb_key[RPMB_KEY_SIZE + 1] = "12345ABCDEF1234512345ABCDEF12345";
static UINT8 rpmb_buffer[RPMB_BLOCK_SIZE];


EFI_STATUS derive_rpmb_key(UINT8 * out_key)
{
	SHA256_CTX sha_ctx;
	char * serialno;
	EFI_STATUS ret;
	UINT8 random[RPMB_KEY_SIZE] = {0};

	if (1 != SHA256_Init(&sha_ctx))
		return EFI_INVALID_PARAMETER;
	serialno = get_serial_number();
	ret = generate_random_numbers(random, RPMB_KEY_SIZE);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to generate random numbers");
		return ret;
	}

	SHA256_Update(&sha_ctx, random, RPMB_KEY_SIZE);
	SHA256_Update(&sha_ctx, serialno,
                                strlen((const CHAR8 *)serialno));
	SHA256_Final(out_key, &sha_ctx);
	OPENSSL_cleanse(&sha_ctx, sizeof(sha_ctx));

	return EFI_SUCCESS;
}

void clear_rpmb_key(void)
{
	memset(rpmb_key, 0, RPMB_KEY_SIZE);
}

void set_rpmb_key(UINT8 *key)
{
	memcpy(rpmb_key, key, RPMB_KEY_SIZE);
}

BOOLEAN is_rpmb_programed(void)
{
	return rpmb_ops.is_rpmb_programed();
}

EFI_STATUS program_rpmb_key(UINT8 *key)
{
	return rpmb_ops.program_rpmb_key(key);
}

EFI_STATUS write_rpmb_device_state(UINT8 state)
{
	return rpmb_ops.write_rpmb_device_state(state);
}

EFI_STATUS read_rpmb_device_state(UINT8 *state)
{
	return rpmb_ops.read_rpmb_device_state(state);
}

EFI_STATUS write_rpmb_rollback_index(size_t index, UINT64 in_rollback_index)
{
	return rpmb_ops.write_rpmb_rollback_index(index, in_rollback_index);
}

EFI_STATUS read_rpmb_rollback_index(size_t index, UINT64 *out_rollback_index)
{
	return rpmb_ops.read_rpmb_rollback_index(index, out_rollback_index);
}

static BOOLEAN is_rpmb_programed_real(void)
{
	EFI_STATUS ret;
	UINT32 write_counter;
	RPMB_RESPONSE_RESULT rpmb_result;

	ret = emmc_get_counter(NULL, &write_counter, (const void *)rpmb_key, &rpmb_result);
	debug(L"get_counter ret=%d, wc=%d", ret, write_counter);
	if (EFI_ERROR(ret) && (rpmb_result == RPMB_RES_NO_AUTH_KEY_PROGRAM)) {
		debug(L"rpmb key is not programmed");
		return FALSE;
	}
	return TRUE;
}

static EFI_STATUS program_rpmb_key_real(UINT8 *key)
{
	EFI_STATUS ret;
	RPMB_RESPONSE_RESULT rpmb_result;

	memcpy(rpmb_key, key, RPMB_KEY_SIZE);
	ret = emmc_program_key(NULL, (const void *)key, &rpmb_result);

	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to program rpmb key");
		return ret;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS write_rpmb_device_state_real(UINT8 state)
{
	EFI_STATUS ret;
	RPMB_RESPONSE_RESULT rpmb_result;

	ret = emmc_read_rpmb_data(NULL, RPMB_DEVICE_STATE_BLOCK_COUNT, RPMB_DEVICE_STATE_BLOCK_ADDR, rpmb_buffer, rpmb_key, &rpmb_result);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read device state");
		return ret;
	}

	rpmb_buffer[0] = DEVICE_STATE_MAGIC;
	rpmb_buffer[1] = state;
	ret = emmc_write_rpmb_data(NULL, RPMB_DEVICE_STATE_BLOCK_COUNT, RPMB_DEVICE_STATE_BLOCK_ADDR, rpmb_buffer, rpmb_key, &rpmb_result);
	debug(L"ret=%d, rpmb_result=%d", ret, rpmb_result);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write device state");
		return ret;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS read_rpmb_device_state_real(UINT8 *state)
{
	EFI_STATUS ret;
	RPMB_RESPONSE_RESULT rpmb_result;

	ret = emmc_read_rpmb_data(NULL, RPMB_DEVICE_STATE_BLOCK_COUNT, RPMB_DEVICE_STATE_BLOCK_ADDR, rpmb_buffer, rpmb_key, &rpmb_result);
	debug(L"ret=%d, rpmb_result=%d", ret, rpmb_result);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read device state");
		return ret;
	}

	if (rpmb_buffer[0] != DEVICE_STATE_MAGIC) {
		return EFI_NOT_FOUND;
	}
	*state = rpmb_buffer[1];
	debug(L"magic=%2x,state=%2x", rpmb_buffer[0], rpmb_buffer[1]);
	return EFI_SUCCESS;
}

static EFI_STATUS write_rpmb_rollback_index_real(size_t index, UINT64 in_rollback_index)
{
	EFI_STATUS ret;
	RPMB_RESPONSE_RESULT rpmb_result;
	UINT16 blk_addr = RPMB_ROLLBACK_INDEX_BLOCK_ADDR;
	UINT16 blk_offset;

        blk_addr += index / RPMB_ROLLBACK_INDEX_COUNT_PER_BLOCK;
        blk_offset = (index % RPMB_ROLLBACK_INDEX_COUNT_PER_BLOCK) * sizeof(UINT64);

	ret = emmc_read_rpmb_data(NULL, 1, blk_addr, rpmb_buffer, rpmb_key, &rpmb_result);
	debug(L"ret=%d, rpmb_result=%d", ret, rpmb_result);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read rollback index");
		return ret;
	}

        if (!memcmp(&in_rollback_index, rpmb_buffer + blk_offset, sizeof(UINT64))) {
		return EFI_SUCCESS;
	}

        memcpy(rpmb_buffer + blk_offset, &in_rollback_index, sizeof(UINT64));
	ret = emmc_write_rpmb_data(NULL, 1, blk_addr, rpmb_buffer, rpmb_key, &rpmb_result);
	debug(L"ret=%d, rpmb_result=%d", ret, rpmb_result);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write rollback index");
		return ret;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS read_rpmb_rollback_index_real(size_t index, UINT64 *out_rollback_index)
{
	EFI_STATUS ret;
	RPMB_RESPONSE_RESULT rpmb_result;
	UINT16 blk_addr = RPMB_ROLLBACK_INDEX_BLOCK_ADDR;
	UINT16 blk_offset;

        blk_addr += index / RPMB_ROLLBACK_INDEX_COUNT_PER_BLOCK;
        blk_offset = (index % RPMB_ROLLBACK_INDEX_COUNT_PER_BLOCK) * sizeof(UINT64);
	ret = emmc_read_rpmb_data(NULL, 1, blk_addr, rpmb_buffer, rpmb_key, &rpmb_result);
	debug(L"ret=%d, rpmb_result=%d", ret, rpmb_result);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read rollback index");
		return ret;
	}
        memcpy(out_rollback_index, rpmb_buffer + blk_offset, sizeof(UINT64));
	debug(L"rollback index=%16x", *out_rollback_index);
	return EFI_SUCCESS;
}

static BOOLEAN is_rpmb_programed_simulate(void)
{
	EFI_STATUS ret;
	UINT32 write_counter;
	RPMB_RESPONSE_RESULT rpmb_result;

	ret = emmc_simulate_get_counter(&write_counter, (const void *)rpmb_key, &rpmb_result);
	debug(L"get_counter ret=%d, wc=%d", ret, write_counter);
	if (EFI_ERROR(ret) && (rpmb_result == RPMB_RES_NO_AUTH_KEY_PROGRAM)) {
		debug(L"rpmb key is not programmed");
		return FALSE;
	}
	return TRUE;
}

static EFI_STATUS program_rpmb_key_simulate(UINT8 *key)
{
	EFI_STATUS efi_ret;
	RPMB_RESPONSE_RESULT rpmb_result;

	memcpy(rpmb_key, key, RPMB_KEY_SIZE);
	efi_ret = emmc_simulate_program_rpmb_key((const void *)key, &rpmb_result);

	if (EFI_ERROR(efi_ret)) {
		efi_perror(efi_ret, L"Failed to program rpmb key");
		return efi_ret;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS write_rpmb_device_state_simulate(UINT8 state)
{
	EFI_STATUS ret;
	UINT32 byte_offset;

	byte_offset = RPMB_DEVICE_STATE_BLOCK_ADDR * RPMB_BLOCK_SIZE;
	ret = emmc_simulate_read_rpmb_data(byte_offset, rpmb_buffer, RPMB_BLOCK_SIZE);
	/*gpt not updated, force success*/
	if (ret == EFI_NOT_FOUND) {
		return EFI_SUCCESS;
	}
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read device state");
		return ret;
	}

	rpmb_buffer[0] = DEVICE_STATE_MAGIC;
	rpmb_buffer[1] = state;
	ret = emmc_simulate_write_rpmb_data(byte_offset, rpmb_buffer, RPMB_BLOCK_SIZE);
	debug(L"ret=%d", ret);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write device state");
		return ret;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS read_rpmb_device_state_simulate(UINT8 *state)
{
	EFI_STATUS ret;
	UINT32 byte_offset;

	byte_offset = RPMB_DEVICE_STATE_BLOCK_ADDR * RPMB_BLOCK_SIZE;
	ret = emmc_simulate_read_rpmb_data(byte_offset, rpmb_buffer, RPMB_BLOCK_SIZE);
	debug(L"ret=%d", ret);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read device state");
		return ret;
	}

	if (rpmb_buffer[0] != DEVICE_STATE_MAGIC) {
		return EFI_NOT_FOUND;
	}
	*state = rpmb_buffer[1];
	debug(L"magic=%2x,state=%2x", rpmb_buffer[0], rpmb_buffer[1]);
	return EFI_SUCCESS;
}

static EFI_STATUS write_rpmb_rollback_index_simulate(size_t index, UINT64 in_rollback_index)
{
	EFI_STATUS ret;
	UINT32 byte_offset;

	byte_offset = RPMB_ROLLBACK_INDEX_BLOCK_ADDR * RPMB_BLOCK_SIZE + index * sizeof(UINT64);

	ret = emmc_simulate_read_rpmb_data(byte_offset, rpmb_buffer, sizeof(UINT64));
	debug(L"ret=%d", ret);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read rollback index");
		return ret;
	}

	/*gpt not updated, force success*/
	if (ret == EFI_NOT_FOUND) {
		return EFI_SUCCESS;
	}

        if (!memcmp(&in_rollback_index, rpmb_buffer, sizeof(UINT64))) {
		return EFI_SUCCESS;
	}

        memcpy(rpmb_buffer, &in_rollback_index, sizeof(UINT64));
	ret = emmc_simulate_write_rpmb_data(byte_offset, rpmb_buffer, sizeof(UINT64));
	debug(L"ret=%d", ret);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write rollback index");
		return ret;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS read_rpmb_rollback_index_simulate(size_t index, UINT64 *out_rollback_index)
{
	EFI_STATUS ret;
	UINT32 byte_offset;

	byte_offset = RPMB_ROLLBACK_INDEX_BLOCK_ADDR * RPMB_BLOCK_SIZE + index * sizeof(UINT64);
	ret = emmc_simulate_read_rpmb_data(byte_offset, rpmb_buffer, sizeof(UINT64));
	debug(L"ret=%d", ret);
	/*gpt not updated, force success*/
	if (ret == EFI_NOT_FOUND) {
		*out_rollback_index = 0;
		return EFI_SUCCESS;
	}
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read rollback index");
		return ret;
	}
        memcpy(out_rollback_index, rpmb_buffer, sizeof(UINT64));
	debug(L"rollback index=%16x", *out_rollback_index);
	return EFI_SUCCESS;
}

void rpmb_storage_init(BOOLEAN real)
{
	if (real) {
		rpmb_ops.is_rpmb_programed = is_rpmb_programed_real;
		rpmb_ops.program_rpmb_key = program_rpmb_key_real;
		rpmb_ops.write_rpmb_device_state = write_rpmb_device_state_real;
		rpmb_ops.read_rpmb_device_state = read_rpmb_device_state_real;
		rpmb_ops.write_rpmb_rollback_index = write_rpmb_rollback_index_real;
		rpmb_ops.read_rpmb_rollback_index = read_rpmb_rollback_index_real;
	} else {
		rpmb_ops.is_rpmb_programed = is_rpmb_programed_simulate;
		rpmb_ops.program_rpmb_key = program_rpmb_key_simulate;
		rpmb_ops.write_rpmb_device_state = write_rpmb_device_state_simulate;
		rpmb_ops.read_rpmb_device_state = read_rpmb_device_state_simulate;
		rpmb_ops.write_rpmb_rollback_index = write_rpmb_rollback_index_simulate;
		rpmb_ops.read_rpmb_rollback_index = read_rpmb_rollback_index_simulate;
	}
}