/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Test Server
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2011 Vic Lee
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
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>

#include <winpr/crt.h>

#include <freerdp/constants.h>
#include <freerdp/utils/sleep.h>
#include <freerdp/server/rdpsnd.h>

#include "sf_audin.h"
#include "sf_rdpsnd.h"

#include "sfreerdp.h"

static char* test_pcap_file = NULL;
static BOOL test_dump_rfx_realtime = TRUE;

/* HL1, LH1, HH1, HL2, LH2, HH2, HL3, LH3, HH3, LL3 */
static const unsigned int test_quantization_values[] =
{
	6, 6, 6, 6, 7, 7, 8, 8, 8, 9
};

void test_peer_context_new(freerdp_peer* client, testPeerContext* context)
{
	context->rfx_context = rfx_context_new();
	context->rfx_context->mode = RLGR3;
	context->rfx_context->width = client->settings->width;
	context->rfx_context->height = client->settings->height;
	rfx_context_set_pixel_format(context->rfx_context, RDP_PIXEL_FORMAT_R8G8B8);

	context->nsc_context = nsc_context_new();
	nsc_context_set_pixel_format(context->nsc_context, RDP_PIXEL_FORMAT_R8G8B8);

	context->s = stream_new(65536);

	context->icon_x = -1;
	context->icon_y = -1;

	context->vcm = WTSCreateVirtualChannelManager(client);
}

void test_peer_context_free(freerdp_peer* client, testPeerContext* context)
{
	if (context)
	{
		if (context->debug_channel_thread)
		{
			freerdp_thread_stop(context->debug_channel_thread);
			freerdp_thread_free(context->debug_channel_thread);
		}

		stream_free(context->s);
		free(context->icon_data);
		free(context->bg_data);

		rfx_context_free(context->rfx_context);
		nsc_context_free(context->nsc_context);

		if (context->debug_channel)
			WTSVirtualChannelClose(context->debug_channel);

		if (context->audin)
			audin_server_context_free(context->audin);

		if (context->rdpsnd)
			rdpsnd_server_context_free(context->rdpsnd);

		WTSDestroyVirtualChannelManager(context->vcm);
	}
}

static void test_peer_init(freerdp_peer* client)
{
	client->context_size = sizeof(testPeerContext);
	client->ContextNew = (psPeerContextNew) test_peer_context_new;
	client->ContextFree = (psPeerContextFree) test_peer_context_free;
	freerdp_peer_context_new(client);
}

static STREAM* test_peer_stream_init(testPeerContext* context)
{
	stream_clear(context->s);
	stream_set_pos(context->s, 0);
	return context->s;
}

static void test_peer_begin_frame(freerdp_peer* client)
{
	rdpUpdate* update = client->update;
	SURFACE_FRAME_MARKER* fm = &update->surface_frame_marker;
	testPeerContext* context = (testPeerContext*) client->context;

	fm->frameAction = SURFACECMD_FRAMEACTION_BEGIN;
	fm->frameId = context->frame_id;
	update->SurfaceFrameMarker(update->context, fm);
}

static void test_peer_end_frame(freerdp_peer* client)
{
	rdpUpdate* update = client->update;
	SURFACE_FRAME_MARKER* fm = &update->surface_frame_marker;
	testPeerContext* context = (testPeerContext*) client->context;

	fm->frameAction = SURFACECMD_FRAMEACTION_END;
	fm->frameId = context->frame_id;
	update->SurfaceFrameMarker(update->context, fm);

	context->frame_id++;
}

static void test_peer_draw_background(freerdp_peer* client)
{
	int size;
	STREAM* s;
	RFX_RECT rect;
	BYTE* rgb_data;
	rdpUpdate* update = client->update;
	SURFACE_BITS_COMMAND* cmd = &update->surface_bits_command;
	testPeerContext* context = (testPeerContext*) client->context;

	if (!client->settings->RemoteFxCodec && !client->settings->NSCodec)
		return;

	test_peer_begin_frame(client);

	s = test_peer_stream_init(context);

	rect.x = 0;
	rect.y = 0;
	rect.width = client->settings->width;
	rect.height = client->settings->height;

	size = rect.width * rect.height * 3;
	rgb_data = malloc(size);
	memset(rgb_data, 0xA0, size);

	if (client->settings->RemoteFxCodec)
	{
		rfx_compose_message(context->rfx_context, s,
			&rect, 1, rgb_data, rect.width, rect.height, rect.width * 3);
		cmd->codecID = client->settings->RemoteFxCodecId;
	}
	else
	{
		nsc_compose_message(context->nsc_context, s,
			rgb_data, rect.width, rect.height, rect.width * 3);
		cmd->codecID = client->settings->NSCodecId;
	}

	cmd->destLeft = 0;
	cmd->destTop = 0;
	cmd->destRight = rect.width;
	cmd->destBottom = rect.height;
	cmd->bpp = 32;
	cmd->width = rect.width;
	cmd->height = rect.height;
	cmd->bitmapDataLength = stream_get_length(s);
	cmd->bitmapData = stream_get_head(s);
	update->SurfaceBits(update->context, cmd);

	free(rgb_data);

	test_peer_end_frame(client);
}

static void test_peer_load_icon(freerdp_peer* client)
{
	testPeerContext* context = (testPeerContext*) client->context;
	FILE* fp;
	int i;
	char line[50];
	BYTE* rgb_data;
	int c;

	if (!client->settings->RemoteFxCodec && !client->settings->NSCodec)
		return;

	if ((fp = fopen("test_icon.ppm", "r")) == NULL)
		return;

	/* P3 */
	fgets(line, sizeof(line), fp);
	/* Creater comment */
	fgets(line, sizeof(line), fp);
	/* width height */
	fgets(line, sizeof(line), fp);
	sscanf(line, "%d %d", &context->icon_width, &context->icon_height);
	/* Max */
	fgets(line, sizeof(line), fp);

	rgb_data = malloc(context->icon_width * context->icon_height * 3);

	for (i = 0; i < context->icon_width * context->icon_height * 3; i++)
	{
		if (fgets(line, sizeof(line), fp))
		{
			sscanf(line, "%d", &c);
			rgb_data[i] = (BYTE)c;
		}
	}

	context->icon_data = rgb_data;

	/* background with same size, which will be used to erase the icon from old position */
	context->bg_data = malloc(context->icon_width * context->icon_height * 3);
	memset(context->bg_data, 0xA0, context->icon_width * context->icon_height * 3);
}

static void test_peer_draw_icon(freerdp_peer* client, int x, int y)
{
	STREAM* s;
	RFX_RECT rect;
	rdpUpdate* update = client->update;
	SURFACE_BITS_COMMAND* cmd = &update->surface_bits_command;
	testPeerContext* context = (testPeerContext*) client->context;

	if (client->update->dump_rfx)
		return;

	if (!context)
		return;

	if (context->icon_width < 1 || !context->activated)
		return;

	test_peer_begin_frame(client);

	rect.x = 0;
	rect.y = 0;
	rect.width = context->icon_width;
	rect.height = context->icon_height;

	if (context->icon_x >= 0)
	{
		s = test_peer_stream_init(context);
		if (client->settings->RemoteFxCodec)
		{
			rfx_compose_message(context->rfx_context, s,
				&rect, 1, context->bg_data, rect.width, rect.height, rect.width * 3);
			cmd->codecID = client->settings->RemoteFxCodecId;
		}
		else
		{
			nsc_compose_message(context->nsc_context, s,
				context->bg_data, rect.width, rect.height, rect.width * 3);
			cmd->codecID = client->settings->NSCodecId;
		}

		cmd->destLeft = context->icon_x;
		cmd->destTop = context->icon_y;
		cmd->destRight = context->icon_x + context->icon_width;
		cmd->destBottom = context->icon_y + context->icon_height;
		cmd->bpp = 32;
		cmd->width = context->icon_width;
		cmd->height = context->icon_height;
		cmd->bitmapDataLength = stream_get_length(s);
		cmd->bitmapData = stream_get_head(s);
		update->SurfaceBits(update->context, cmd);
	}

	s = test_peer_stream_init(context);

	if (client->settings->RemoteFxCodec)
	{
		rfx_compose_message(context->rfx_context, s,
			&rect, 1, context->icon_data, rect.width, rect.height, rect.width * 3);
		cmd->codecID = client->settings->RemoteFxCodecId;
	}
	else
	{
		nsc_compose_message(context->nsc_context, s,
			context->icon_data, rect.width, rect.height, rect.width * 3);
		cmd->codecID = client->settings->NSCodecId;
	}

	cmd->destLeft = x;
	cmd->destTop = y;
	cmd->destRight = x + context->icon_width;
	cmd->destBottom = y + context->icon_height;
	cmd->bpp = 32;
	cmd->width = context->icon_width;
	cmd->height = context->icon_height;
	cmd->bitmapDataLength = stream_get_length(s);
	cmd->bitmapData = stream_get_head(s);
	update->SurfaceBits(update->context, cmd);

	context->icon_x = x;
	context->icon_y = y;

	test_peer_end_frame(client);
}

static BOOL test_sleep_tsdiff(UINT32 *old_sec, UINT32 *old_usec, UINT32 new_sec, UINT32 new_usec)
{
	INT32 sec, usec;

	if (*old_sec==0 && *old_usec==0)
	{
		*old_sec = new_sec;
		*old_usec = new_usec;
		return TRUE;
	}

	sec = new_sec - *old_sec;
	usec = new_usec - *old_usec;

	if (sec<0 || (sec==0 && usec<0))
	{
		printf("Invalid time stamp detected.\n");
		return FALSE;
	}

	*old_sec = new_sec;
	*old_usec = new_usec;
	
	while (usec < 0) 
	{
		usec += 1000000;
		sec--;
	}
	
	if (sec > 0)
		freerdp_sleep(sec);
	
	if (usec > 0)
		freerdp_usleep(usec);
	
	return TRUE;
}

void tf_peer_dump_rfx(freerdp_peer* client)
{
	STREAM* s;
	UINT32 prev_seconds;
	UINT32 prev_useconds;
	rdpUpdate* update;
	rdpPcap* pcap_rfx;
	pcap_record record;

	s = stream_new(512);
	update = client->update;
	client->update->pcap_rfx = pcap_open(test_pcap_file, FALSE);
	pcap_rfx = client->update->pcap_rfx;

	if (pcap_rfx == NULL)
		return;

	prev_seconds = prev_useconds = 0;

	while (pcap_has_next_record(pcap_rfx))
	{
		pcap_get_next_record_header(pcap_rfx, &record);

		s->data = realloc(s->data, record.length);
		record.data = s->data;
		s->size = record.length;

		pcap_get_next_record_content(pcap_rfx, &record);
		s->p = s->data + s->size;

		if (test_dump_rfx_realtime && test_sleep_tsdiff(&prev_seconds, &prev_useconds, record.header.ts_sec, record.header.ts_usec) == FALSE)
			break;

		update->SurfaceCommand(update->context, s);
	}
}

static void* tf_debug_channel_thread_func(void* arg)
{
	void* fd;
	STREAM* s;
	void* buffer;
	UINT32 bytes_returned = 0;
	testPeerContext* context = (testPeerContext*) arg;
	freerdp_thread* thread = context->debug_channel_thread;

	if (WTSVirtualChannelQuery(context->debug_channel, WTSVirtualFileHandle, &buffer, &bytes_returned) == TRUE)
	{
		fd = *((void**)buffer);
		WTSFreeMemory(buffer);
		thread->signals[thread->num_signals++] = wait_obj_new_with_fd(fd);
	}

	s = stream_new(4096);

	WTSVirtualChannelWrite(context->debug_channel, (BYTE*) "test1", 5, NULL);

	while (1)
	{
		freerdp_thread_wait(thread);

		if (freerdp_thread_is_stopped(thread))
			break;

		stream_set_pos(s, 0);

		if (WTSVirtualChannelRead(context->debug_channel, 0, stream_get_head(s),
			stream_get_size(s), &bytes_returned) == FALSE)
		{
			if (bytes_returned == 0)
				break;

			stream_check_size(s, bytes_returned);

			if (WTSVirtualChannelRead(context->debug_channel, 0, stream_get_head(s),
				stream_get_size(s), &bytes_returned) == FALSE)
			{
				/* should not happen */
				break;
			}
		}

		stream_set_pos(s, bytes_returned);

		printf("got %d bytes\n", bytes_returned);
	}

	stream_free(s);
	freerdp_thread_quit(thread);

	return 0;
}

BOOL tf_peer_post_connect(freerdp_peer* client)
{
	int i;
	testPeerContext* context = (testPeerContext*) client->context;

	/**
	 * This callback is called when the entire connection sequence is done, i.e. we've received the
	 * Font List PDU from the client and sent out the Font Map PDU.
	 * The server may start sending graphics output and receiving keyboard/mouse input after this
	 * callback returns.
	 */

	printf("Client %s is activated (osMajorType %d osMinorType %d)", client->local ? "(local)" : client->hostname,
			client->settings->OsMajorType, client->settings->OsMinorType);

	if (client->settings->AutoLogonEnabled)
	{
		printf(" and wants to login automatically as %s\\%s",
			client->settings->Domain ? client->settings->Domain : "",
			client->settings->Username);

		/* A real server may perform OS login here if NLA is not executed previously. */
	}
	printf("\n");

	printf("Client requested desktop: %dx%dx%d\n",
		client->settings->width, client->settings->height, client->settings->color_depth);

	/* A real server should tag the peer as activated here and start sending updates in mainloop. */
	test_peer_load_icon(client);

	/* Iterate all channel names requested by the client and activate those supported by the server */

	for (i = 0; i < client->settings->ChannelCount; i++)
	{
		if (client->settings->ChannelDefArray[i].joined)
		{
			if (strncmp(client->settings->ChannelDefArray[i].Name, "rdpdbg", 6) == 0)
			{
				context->debug_channel = WTSVirtualChannelOpenEx(context->vcm, "rdpdbg", 0);

				if (context->debug_channel != NULL)
				{
					printf("Open channel rdpdbg.\n");
					context->debug_channel_thread = freerdp_thread_new();
					freerdp_thread_start(context->debug_channel_thread,
						tf_debug_channel_thread_func, context);
				}
			}
			else if (strncmp(client->settings->ChannelDefArray[i].Name, "rdpsnd", 6) == 0)
			{
				sf_peer_rdpsnd_init(context); /* Audio Output */
			}
		}
	}

	/* Dynamic Virtual Channels */

	sf_peer_audin_init(context); /* Audio Input */

	/* Return FALSE here would stop the execution of the peer mainloop. */

	return TRUE;
}

BOOL tf_peer_activate(freerdp_peer* client)
{
	testPeerContext* context = (testPeerContext*) client->context;

	rfx_context_reset(context->rfx_context);
	context->activated = TRUE;

	if (test_pcap_file != NULL)
	{
		client->update->dump_rfx = TRUE;
		tf_peer_dump_rfx(client);
	}
	else
	{
		test_peer_draw_background(client);
	}

	return TRUE;
}

void tf_peer_synchronize_event(rdpInput* input, UINT32 flags)
{
	printf("Client sent a synchronize event (flags:0x%X)\n", flags);
}

void tf_peer_keyboard_event(rdpInput* input, UINT16 flags, UINT16 code)
{
	freerdp_peer* client = input->context->peer;
	rdpUpdate* update = client->update;
	testPeerContext* context = (testPeerContext*) input->context;

	printf("Client sent a keyboard event (flags:0x%X code:0x%X)\n", flags, code);

	if ((flags & 0x4000) && code == 0x22) /* 'g' key */
	{
		if (client->settings->width != 800)
		{
			client->settings->width = 800;
			client->settings->height = 600;
		}
		else
		{
			client->settings->width = 640;
			client->settings->height = 480;
		}
		update->DesktopResize(update->context);
		context->activated = FALSE;
	}
	else if ((flags & 0x4000) && code == 0x2E) /* 'c' key */
	{
		if (context->debug_channel)
		{
			WTSVirtualChannelWrite(context->debug_channel, (BYTE*) "test2", 5, NULL);
		}
	}
	else if ((flags & 0x4000) && code == 0x2D) /* 'x' key */
	{
		client->Close(client);
	}
	else if ((flags & 0x4000) && code == 0x13) /* 'r' key */
	{
		if (!context->audin_open)
		{
			context->audin->Open(context->audin);
			context->audin_open = TRUE;
		}
		else
		{
			context->audin->Close(context->audin);
			context->audin_open = FALSE;
		}
	}
	else if ((flags & 0x4000) && code == 0x1F) /* 's' key */
	{

	}
}

void tf_peer_unicode_keyboard_event(rdpInput* input, UINT16 flags, UINT16 code)
{
	printf("Client sent a unicode keyboard event (flags:0x%X code:0x%X)\n", flags, code);
}

void tf_peer_mouse_event(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	//printf("Client sent a mouse event (flags:0x%X pos:%d,%d)\n", flags, x, y);

	test_peer_draw_icon(input->context->peer, x + 10, y);
}

void tf_peer_extended_mouse_event(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	//printf("Client sent an extended mouse event (flags:0x%X pos:%d,%d)\n", flags, x, y);
}

static void tf_peer_refresh_rect(rdpContext* context, BYTE count, RECTANGLE_16* areas)
{
	BYTE i;

	printf("Client requested to refresh:\n");

	for (i = 0; i < count; i++)
	{
		printf("  (%d, %d) (%d, %d)\n", areas[i].left, areas[i].top, areas[i].right, areas[i].bottom);
	}
}

static void tf_peer_suppress_output(rdpContext* context, BYTE allow, RECTANGLE_16* area)
{
	if (allow > 0)
	{
		printf("Client restore output (%d, %d) (%d, %d).\n", area->left, area->top, area->right, area->bottom);
	}
	else
	{
		printf("Client minimized and suppress output.\n");
	}
}

static void* test_peer_mainloop(void* arg)
{
	int i;
	int fds;
	int max_fds;
	int rcount;
	void* rfds[32];
	fd_set rfds_set;
	testPeerContext* context;
	freerdp_peer* client = (freerdp_peer*) arg;

	memset(rfds, 0, sizeof(rfds));

	test_peer_init(client);

	/* Initialize the real server settings here */
	client->settings->CertificateFile = _strdup("server.crt");
	client->settings->PrivateKeyFile = _strdup("server.key");
	client->settings->NlaSecurity = FALSE;
	client->settings->RemoteFxCodec = TRUE;
	client->settings->SuppressOutput = TRUE;
	client->settings->RefreshRect = TRUE;

	client->PostConnect = tf_peer_post_connect;
	client->Activate = tf_peer_activate;

	client->input->SynchronizeEvent = tf_peer_synchronize_event;
	client->input->KeyboardEvent = tf_peer_keyboard_event;
	client->input->UnicodeKeyboardEvent = tf_peer_unicode_keyboard_event;
	client->input->MouseEvent = tf_peer_mouse_event;
	client->input->ExtendedMouseEvent = tf_peer_extended_mouse_event;

	client->update->RefreshRect = tf_peer_refresh_rect;
	client->update->SuppressOutput = tf_peer_suppress_output;

	client->Initialize(client);
	context = (testPeerContext*) client->context;

	printf("We've got a client %s\n", client->local ? "(local)" : client->hostname);

	while (1)
	{
		rcount = 0;

		if (client->GetFileDescriptor(client, rfds, &rcount) != TRUE)
		{
			printf("Failed to get FreeRDP file descriptor\n");
			break;
		}
		WTSVirtualChannelManagerGetFileDescriptor(context->vcm, rfds, &rcount);

		max_fds = 0;
		FD_ZERO(&rfds_set);

		for (i = 0; i < rcount; i++)
		{
			fds = (int)(long)(rfds[i]);

			if (fds > max_fds)
				max_fds = fds;

			FD_SET(fds, &rfds_set);
		}

		if (max_fds == 0)
			break;

		if (select(max_fds + 1, &rfds_set, NULL, NULL, NULL) == -1)
		{
			/* these are not really errors */
			if (!((errno == EAGAIN) ||
				(errno == EWOULDBLOCK) ||
				(errno == EINPROGRESS) ||
				(errno == EINTR))) /* signal occurred */
			{
				printf("select failed\n");
				break;
			}
		}

		if (client->CheckFileDescriptor(client) != TRUE)
			break;
		if (WTSVirtualChannelManagerCheckFileDescriptor(context->vcm) != TRUE)
			break;
	}

	printf("Client %s disconnected.\n", client->local ? "(local)" : client->hostname);

	client->Disconnect(client);
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);

	return NULL;
}

static void test_peer_accepted(freerdp_listener* instance, freerdp_peer* client)
{
	pthread_t th;

	pthread_create(&th, 0, test_peer_mainloop, client);
	pthread_detach(th);
}

static void test_server_mainloop(freerdp_listener* instance)
{
	int i;
	int fds;
	int max_fds;
	int rcount;
	void* rfds[32];
	fd_set rfds_set;

	memset(rfds, 0, sizeof(rfds));

	while (1)
	{
		rcount = 0;

		if (instance->GetFileDescriptor(instance, rfds, &rcount) != TRUE)
		{
			printf("Failed to get FreeRDP file descriptor\n");
			break;
		}

		max_fds = 0;
		FD_ZERO(&rfds_set);

		for (i = 0; i < rcount; i++)
		{
			fds = (int)(long)(rfds[i]);

			if (fds > max_fds)
				max_fds = fds;

			FD_SET(fds, &rfds_set);
		}

		if (max_fds == 0)
			break;

		if (select(max_fds + 1, &rfds_set, NULL, NULL, NULL) == -1)
		{
			/* these are not really errors */
			if (!((errno == EAGAIN) ||
				(errno == EWOULDBLOCK) ||
				(errno == EINPROGRESS) ||
				(errno == EINTR))) /* signal occurred */
			{
				printf("select failed\n");
				break;
			}
		}

		if (instance->CheckFileDescriptor(instance) != TRUE)
		{
			printf("Failed to check FreeRDP file descriptor\n");
			break;
		}
	}

	instance->Close(instance);
}

int main(int argc, char* argv[])
{
	freerdp_listener* instance;

	/* Ignore SIGPIPE, otherwise an SSL_write failure could crash your server */
	signal(SIGPIPE, SIG_IGN);

	instance = freerdp_listener_new();

	instance->PeerAccepted = test_peer_accepted;

	if (argc > 1)
		test_pcap_file = argv[1];
	
	if (argc > 2 && !strcmp(argv[2], "--fast"))
		test_dump_rfx_realtime = FALSE;

	/* Open the server socket and start listening. */
	if (instance->Open(instance, NULL, 3389) &&
		instance->OpenLocal(instance, "/tmp/tfreerdp-server.0"))
	{
		/* Entering the server main loop. In a real server the listener can be run in its own thread. */
		test_server_mainloop(instance);
	}

	freerdp_listener_free(instance);

	return 0;
}

