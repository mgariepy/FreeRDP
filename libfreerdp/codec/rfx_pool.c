/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RemoteFX Codec Library - Memory Pool
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>

#include "rfx_pool.h"

RFX_POOL* rfx_pool_new()
{
	RFX_POOL* pool;

	pool = (RFX_POOL*) malloc(sizeof(RFX_POOL));
	ZeroMemory(pool, sizeof(RFX_POOL));

	pool->size = 64;
	pool->tiles = (RFX_TILE**) malloc(sizeof(RFX_TILE*) * pool->size);
	ZeroMemory(pool->tiles, sizeof(RFX_TILE*) * pool->size);

	return pool;
}

void rfx_pool_free(RFX_POOL* pool)
{
	int i;
	RFX_TILE* tile;

	for (i = 0; i < pool->count; i++)
	{
		tile = pool->tiles[i];

		if (tile != NULL)
		{
			if (tile->data != NULL)
				free(tile->data);

			free(tile);
		}
	}

	free(pool->tiles);
	free(pool);
}

void rfx_pool_put_tile(RFX_POOL* pool, RFX_TILE* tile)
{
	if (pool->count >= pool->size)
	{
		pool->size *= 2;
		pool->tiles = (RFX_TILE**) realloc((void*) pool->tiles, sizeof(RFX_TILE*) * pool->size);
	}

	pool->tiles[(pool->count)++] = tile;
}

RFX_TILE* rfx_pool_get_tile(RFX_POOL* pool)
{
	RFX_TILE* tile;

	if (pool->count < 1)
	{
		tile = (RFX_TILE*) malloc(sizeof(RFX_TILE));
		ZeroMemory(tile, sizeof(RFX_TILE));

		tile->data = (BYTE*) malloc(4096 * 4); /* 64x64 * 4 */
	}
	else
	{
		tile = pool->tiles[--(pool->count)];
	}

	return tile;
}

void rfx_pool_put_tiles(RFX_POOL* pool, RFX_TILE** tiles, int count)
{
	int i;

	for (i = 0; i < count; i++)
	{
		rfx_pool_put_tile(pool, tiles[i]);
	}
}

RFX_TILE** rfx_pool_get_tiles(RFX_POOL* pool, int count)
{
	int i;
	RFX_TILE** tiles;

	tiles = (RFX_TILE**) malloc(sizeof(RFX_TILE*) * count);

	for (i = 0; i < count; i++)
	{
		tiles[i] = rfx_pool_get_tile(pool);
	}

	return tiles;
}
