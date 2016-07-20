/*
 *  Squeeze2raop - LMS to RAOP gateway
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#if WIN
#include <process.h>
#endif

#include "ixml.h"
#include "squeezedefs.h"
#include "squeeze2raop.h"
#include "conf_util.h"
#include "util_common.h"
#include "util.h"

#include "mdnssd-itf.h"
#include "http_fetcher.h"
#include "mdns.h"
#include "mdnsd.h"


/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
char				glInterface[16] = "?";
#if LINUX || FREEBSD
bool				glDaemonize = false;
#endif
bool				glInteractive = true;
char				*glLogFile;
s32_t				glLogLimit = -1;
static char			*glPidFile = NULL;
static char			*glSaveConfigFile = NULL;
bool				glAutoSaveConfigFile = false;
bool				glGracefullShutdown = true;
int					gl_mDNSId;
char 				glDACPid[] = "1A2B3D4EA1B2C3D4";

log_level	slimproto_loglevel = lINFO;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lINFO;
log_level	main_loglevel = lINFO;
log_level	slimmain_loglevel = lINFO;
log_level	util_loglevel = lINFO;
log_level	raop_loglevel = lINFO;

tMRConfig			glMRConfig = {
							true,
							"",
							false,
							false,
							30,
							false,
							60,
							false,
					};

static u8_t LMSVolumeMap[101] = {
				0, 1, 1, 1, 2, 2, 2, 3,  3,  4,
				5, 5, 6, 6, 7, 8, 9, 9, 10, 11,
				12, 13, 14, 15, 16, 16, 17, 18, 19, 20,
				22, 23, 24, 25, 26, 27, 28, 29, 30, 32,
				33, 34, 35, 37, 38, 39, 40, 42, 43, 44,
				46, 47, 48, 50, 51, 53, 54, 56, 57, 59,
				60, 61, 63, 65, 66, 68, 69, 71, 72, 74,
				75, 77, 79, 80, 82, 84, 85, 87, 89, 90,
				92, 94, 96, 97, 99, 101, 103, 104, 106, 108, 110,
				112, 113, 115, 117, 119, 121, 123, 125, 127, 128
			};

sq_dev_param_t glDeviceParam = {
					STREAMBUF_SIZE,
					OUTPUTBUF_SIZE,
					"flc,pcm,aif,aac,mp3",
					"?",
					"",
#if defined(RESAMPLE)
					SQ_RATE_96000,
#else
					SQ_RATE_44100,
#endif
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
					false,
					3000,
#if defined(RESAMPLE)
					true,
					"",
#endif
				} ;

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
static pthread_t 	glMainThread;
void				*glConfigID = NULL;
char				glConfigName[SQ_STR_LENGTH] = "./config.xml";
static bool			glDiscovery = false;
u32_t				glScanInterval = SCAN_INTERVAL;
u32_t				glScanTimeout = SCAN_TIMEOUT;
struct sMR			glMRDevices[MAX_RENDERERS];

/*----------------------------------------------------------------------------*/
/* local types */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals variables */
/*----------------------------------------------------------------------------*/
extern log_level	main_loglevel;
static log_level 	*loglevel = &main_loglevel;

static pthread_t			glUpdateMRThread;
static bool			glMainRunning = true;

static struct mdnsd *gl_mDNSResponder;
static int			glActiveRemoteSock;
static pthread_t	glActiveRemoteThread;

static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
  		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output|web|main|util|raop, level: error|warn|info|debug|sdebug\n"
#if LINUX || FREEBSD
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
	;

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *UpdateMRThread(void *args);
static bool AddRaopDevice(struct sMR *Device, struct mDNSItem_s *data);
void 		DelRaopDevice(struct sMR *Device);

/*----------------------------------------------------------------------------*/
bool sq_callback(sq_dev_handle_t handle, void *caller, sq_action_t action, u8_t *cookie, void *param)
{
	struct sMR *device = caller;
	char *p = (char*) param;
	bool rc = true;

	if (!device)	{
		LOG_ERROR("No caller ID in callback", NULL);
		return false;
	}

	if (action == SQ_ONOFF) {
		device->on = *((bool*) param);

		if (device->on && device->Config.AutoPlay)
			sq_notify(device->SqueezeHandle, device, SQ_PLAY, NULL, &device->on);

		if (!device->on) {
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			pthread_mutex_lock(&device->Mutex);
			QueueFlush(&device->Queue);
			strcpy(Req->Type, "OFF");
			QueueInsert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
			pthread_mutex_unlock(&device->Mutex);
		}

		LOG_DEBUG("[%p]: device set on/off %d", caller, device->on);
	}

	if (!device->on) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		return false;
	}

	LOG_SDEBUG("callback for %s (%d)", device->FriendlyName, action);
	pthread_mutex_lock(&device->Mutex);

	switch (action) {
		case SQ_FINISHED:
			device->LastFlush = gettime_ms();
			device->TearDownWait = true;
			// will miss the last "buffer time" track position updates on the player
			device->TrackDuration = -1;
			break;
		case SQ_STOP:
			device->TrackDuration = -1;
		case SQ_PAUSE: {
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			strcpy(Req->Type, "FLUSH");
			Req->Data.FlushMode = (action != SQ_PAUSE) ? RAOP_REBUFFER : RAOP_RECLOCK;
			QueueInsert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
			break;
		}
		case SQ_UNPAUSE: {
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			Req->Data.Codec = RAOP_NOCODEC;
			strcpy(Req->Type, "CONNECT");
			QueueInsert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
			break;
		}
		case SQ_VOLUME: {
			u32_t Volume = *(u16_t*)p;
			tRaopReq *Req = malloc(sizeof(tRaopReq));
			int i;

			for (i = 100; Volume < LMSVolumeMap[i] && i; i--);
			device->Volume = i;
			if (Volume > 100) Volume = 100;

			Req->Data.Volume = Volume;
			strcpy(Req->Type, "VOLUME");
			QueueInsert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);

			break;
		}
		case SQ_CONNECT: {
			sq_format_t *p = (sq_format_t*) param;
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			Req->Data.Codec = RAOP_ALAC;
			strcpy(Req->Type, "CONNECT");
			QueueInsert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);

			LOG_INFO("[%p]: codec:%c, ch:%d, s:%d, r:%d", device, p->codec,
										p->channels, p->sample_size, p->sample_rate);
			break;
		}
		case SQ_METASEND:
			device->MetaDataWait = 5;
			break;
		case SQ_STARTED:
			device->TrackStartTime = *((u32_t*) param);
			device->MetaDataWait = 1;
			break;
		case SQ_SETNAME:
			strcpy(device->sq_config.name, param);
			break;
		default:
			break;
	}

	pthread_mutex_unlock(&device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
static void *PlayerThread(void *args)
{
	struct sMR *Device = (struct sMR*) args;

	Device->Running = true;

	while (Device->Running) {
		tRaopReq *req = GetRequest(&Device->Queue, &Device->Mutex, &Device->Cond, 1000);

		// empty means timeout every sec
		if (!req) {
			u32_t now = gettime_ms();

			LOG_DEBUG("[%p]: tick %u", Device, now);
			if (Device->TearDownWait && now > Device->LastFlush + Device->Config.IdleTimeout * 1000) {
				LOG_INFO("[%p]: Tear down connection %u", Device, now);
				raopcl_disconnect(Device->Raop);
				Device->TearDownWait = false;
			}

			if (!raopcl_is_sane(Device->Raop)) {
				LOG_ERROR("[%p]: broken connection, attempting reset", Device);
				raopcl_disconnect(Device->Raop);
				raopcl_reconnect(Device->Raop);
			}

			if (Device->Config.SendMetaData && Device->TrackDuration != -1) {
				raopcl_progress(Device->Raop, Device->TrackStartTime, sq_position_ms(Device->SqueezeHandle, NULL), Device->TrackDuration);
			}

			pthread_mutex_lock(&Device->Mutex);
			if (Device->MetaDataWait && !--Device->MetaDataWait && Device->Config.SendMetaData) {
				sq_metadata_t metadata;

				pthread_mutex_unlock(&Device->Mutex);
				LOG_INFO("[%p]: getting metadata", Device);
				sq_get_metadata(Device->SqueezeHandle, &metadata, false);
				Device->TrackDuration = metadata.duration;
				raopcl_set_daap(Device->Raop, 5, "minm", 's', metadata.title,
												 "asar", 's', metadata.artist,
												 "asal", 's', metadata.album,
												 "asgn", 's', metadata.genre,
												 "astn", 'i', (int) metadata.track);
				// TODO: check that it's JPEG
				if (metadata.artwork && Device->Config.SendCoverArt) {
					char *image = NULL, *contentType = NULL;
					int size = http_fetch(metadata.artwork, &contentType, &image);

					if (size != -1) raopcl_set_artwork(Device->Raop, contentType, size - 1, image);

					NFREE(image);
					NFREE(contentType);
				}
				sq_free_metadata(&metadata);
			}
			else pthread_mutex_unlock(&Device->Mutex);

			continue;
		}

		if (!strcasecmp(req->Type, "CONNECT")) {
			LOG_INFO("[%p]: raop connecting ...", Device);
			if (raopcl_connect(Device->Raop, Device->PlayerIP, Device->PlayerPort, req->Data.Codec)) {
				Device->TearDownWait = false;
				LOG_INFO("[%p]: raop connected", Device);
			}
			else {
				LOG_ERROR("[%p]: raop failed to connect", Device);
			}
		}

		if (!strcasecmp(req->Type, "FLUSH")) {
			LOG_INFO("[%p]: flushing ... (%d)", Device, req->Data.FlushMode);
			Device->LastFlush = gettime_ms();
			Device->TearDownWait = true;
			raopcl_flush_stream(Device->Raop, req->Data.FlushMode);
		}

		if (!strcasecmp(req->Type, "OFF")) {
			LOG_INFO("[%p]: processing off", Device);
			raopcl_disconnect(Device->Raop);
		}

		if (!strcasecmp(req->Type, "VOLUME")) {
			LOG_INFO("[%p]: processing volume", Device);
			raopcl_update_volume(Device->Raop, req->Data.Volume, false);
		}

		free(req);

	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool RefreshTO(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].InUse && !strcmp(glMRDevices[i].UDN, UDN)) {
			glMRDevices[i].TimeOut = false;
			glMRDevices[i].MissingCount = glMRDevices[i].Config.RemoveCount;
			return true;
		}
	}
	return false;
}


/*----------------------------------------------------------------------------*/
char *GetmDNSAttribute(struct mDNSItem_s *p, char *name)
{
	int j;

	for (j = 0; j < p->attr_count; j++)
		if (!strcasecmp(p->attr[j].name, name))
			return strdup(p->attr[j].value);

	return NULL;
}


/*----------------------------------------------------------------------------*/
static void *UpdateMRThread(void *args)
{
	struct sMR *Device = NULL;
	int i, TimeStamp;
	DiscoveredList DiscDevices;
	static bool Running = false;

	if (Running) return NULL;
	Running = true;

	LOG_DEBUG("Begin Raop devices update", NULL);
	TimeStamp = gettime_ms();

	query_mDNS(gl_mDNSId, "_raop._tcp.local", &DiscDevices, glScanTimeout);

	if (!glMainRunning) {
		LOG_DEBUG("Aborting ...", NULL);
		Running = false;
		return NULL;
	}

	for (i = 0; i < DiscDevices.count; i++) {
		int j;
		struct mDNSItem_s *p = &DiscDevices.items[i];

		if (!p->name) continue;

		if (!RefreshTO(p->name)) {
			// new device so search a free spot.
			for (j = 0; j < MAX_RENDERERS && glMRDevices[j].InUse; j++);

			// no more room !
			if (j == MAX_RENDERERS) {
				LOG_ERROR("Too many Raop devices", NULL);
				break;
			}
			else Device = &glMRDevices[j];

			if (AddRaopDevice(Device, p) && !glSaveConfigFile) {
				// create a new slimdevice
				Device->SqueezeHandle = sq_reserve_device(Device, &sq_callback);
				if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
				if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle,
					Device->Raop,
					&Device->sq_config,
					Device->SampleRate ? atoi(Device->SampleRate) : 44100,
					Device->SampleSize ? atoi(Device->SampleSize) : 16)) {
					sq_release_device(Device->SqueezeHandle);
					Device->SqueezeHandle = 0;
					LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
					DelRaopDevice(Device);
				}
			}
		}
		else {
			for (j = 0; j < MAX_RENDERERS; j++) {
				if (glMRDevices[j].InUse && !strcmp(glMRDevices[j].UDN, p->name)) {
					if (p->port != glMRDevices[j].PlayerPort || p->addr.s_addr != glMRDevices[j].PlayerIP.s_addr) {
						LOG_INFO("[%p]: changed ip:port %s:%d", &glMRDevices[j], inet_ntoa(p->addr), p->port);
						glMRDevices[j].PlayerPort = p->port;
						glMRDevices[j].PlayerIP = p->addr;
					}
					break;
				}
			}
		}
	}

	free_discovered_list(&DiscDevices);

	// then walk through the list of devices to remove missing ones
	for (i = 0; i < MAX_RENDERERS; i++) {
		Device = &glMRDevices[i];
		if (!Device->InUse) continue;
		if (Device->TimeOut && Device->MissingCount) Device->MissingCount--;
		// LOG_INFO("[%p]: missing %d %s", Device, Device->MissingCount, Device->FriendlyName);
		if (Device->MissingCount) continue;

		LOG_INFO("[%p]: removing renderer (%s)", Device, Device->FriendlyName);
		if (Device->SqueezeHandle) sq_delete_device(Device->SqueezeHandle);
		DelRaopDevice(Device);
	}

	glDiscovery = true;

	if (glAutoSaveConfigFile && !glSaveConfigFile) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	LOG_DEBUG("End Raop devices update %d", gettime_ms() - TimeStamp);
	Running = false;
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	unsigned last = gettime_ms();
	u32_t ScanPoll = glScanInterval*1000 + 1;

	while (glMainRunning) {
		int i;
		int elapsed = gettime_ms() - last;

		// reset timeout and re-scan devices
		ScanPoll += elapsed;
		if (glScanInterval && ScanPoll > glScanInterval*1000) {
			pthread_attr_t attr;

			ScanPoll = 0;

			for (i = 0; i < MAX_RENDERERS; i++) {
				glMRDevices[i].TimeOut = true;
				glDiscovery = false;
			}

			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 64*1024);
			pthread_create(&glUpdateMRThread, &attr, &UpdateMRThread, NULL);
			pthread_detach(glUpdateMRThread);
			pthread_attr_destroy(&attr);
		}

		if (glLogFile && glLogLimit != - 1) {
			s32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				u32_t Sum, BufSize = 16384;
				u8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
			}
		}

		last = gettime_ms();
		sleep(1);
	}
	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool AddRaopDevice(struct sMR *Device, struct mDNSItem_s *data)
{
	pthread_attr_t attr;
	raop_crypto_t Crypto;
	char *p;
	u32_t mac_size = 6;

	// read parameters from default then config file
	memset(Device, 0, sizeof(struct sMR));
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, data->name, &Device->Config, &Device->sq_config);

	if (!Device->Config.Enabled) return false;

#if 0
	if (!stristr(data->name, "JBL")) {
		printf("ONLY JBL %s\n", data->name);
		return false;
	}
#endif

	strcpy(Device->UDN, data->name);
	Device->Magic = MAGIC;
	Device->TimeOut = false;
	Device->MissingCount = Device->Config.RemoveCount;
	Device->on = false;
	Device->SqueezeHandle = 0;
	Device->Running = true;
	Device->InUse = true;
	Device->VolumeStamp = 0;
	Device->PlayerIP = data->addr;
	Device->PlayerPort = data->port;
	Device->TearDownWait = false;
	Device->TrackDuration = -1;
	sprintf(Device->ActiveRemote, "%d", hash32(Device->UDN));
	strcpy(Device->FriendlyName, data->hostname);
	p = stristr(Device->FriendlyName, ".local");
	if (p) *p = '\0';

	LOG_INFO("[%p]: adding renderer (%s)", Device, Device->FriendlyName);

	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", mac_size)) {
		if (SendARP(*((in_addr_t*) &Device->PlayerIP), INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			u32_t hash = hash32(Device->UDN);

			LOG_ERROR("[%p]: cannot get mac %s, creating fake %x", Device, Device->FriendlyName, hash);
			memcpy(Device->sq_config.mac + 2, &hash, 4);
		}
		memset(Device->sq_config.mac, 0xaa, 2);
	}

	// gather RAOP device capabilities, to be macthed mater
	Device->SampleSize = GetmDNSAttribute(data, "ss");
	Device->SampleRate = GetmDNSAttribute(data, "sr");
	Device->Channels = GetmDNSAttribute(data, "ch");
	Device->Codecs = GetmDNSAttribute(data, "cn");
	Device->Crypto = GetmDNSAttribute(data, "et");

	if (!strchr(Device->Codecs, '0')) {
		LOG_ERROR("[%p]: incompatible codec", Device->Codecs);
		return false;
	}

	if ((Device->Config.Encryption && strchr(Device->Crypto, '1')) ||
		(!strchr(Device->Crypto, '0') && strchr(Device->Crypto, '1')))
		Crypto = RAOP_RSA;
	else
		Crypto = RAOP_CLEAR;

	Device->Raop = raopcl_create(glInterface, glDACPid, Device->ActiveRemote, RAOP_ALAC, Crypto,
								 Device->SampleRate ? atoi(Device->SampleRate) : 44100,
								 Device->SampleSize ? atoi(Device->SampleSize) : 16,
								 Device->Channels ? atoi(Device->Channels) : 2,
								 abs(Device->sq_config.player_volume));

	if (!Device->Raop) {
		LOG_ERROR("[%p]: cannot create raop device", Device);
		NFREE(Device->SampleSize);
		NFREE(Device->SampleRate);
		NFREE(Device->Channels);
		NFREE(Device->Codecs);
		NFREE(Device->Crypto);
		return false;
	}

	pthread_mutex_init(&Device->Mutex, 0);
	pthread_cond_init(&Device->Cond, 0);
	QueueInit(&Device->Queue);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 64*1024);
	pthread_create(&Device->Thread, NULL, &PlayerThread, Device);
	pthread_attr_destroy(&attr);

	return true;
}


/*----------------------------------------------------------------------------*/
void FlushRaopDevices(void)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->InUse) DelRaopDevice(p);
	}
}


/*----------------------------------------------------------------------------*/
void DelRaopDevice(struct sMR *Device)
{
	pthread_mutex_lock(&Device->Mutex);
	Device->Running = false;
	Device->InUse = false;
	pthread_cond_signal(&Device->Cond);
	pthread_mutex_unlock(&Device->Mutex);

	pthread_join(Device->Thread, NULL);

	pthread_cond_destroy(&Device->Cond);
	pthread_mutex_destroy(&Device->Mutex);

	raopcl_destroy(Device->Raop);

	LOG_INFO("[%p]: Raop device stopped", Device);

	NFREE(Device->SampleSize);
	NFREE(Device->SampleRate);
	NFREE(Device->Channels);
	NFREE(Device->Codecs);
	NFREE(Device->Crypto);

	memset(Device, 0, sizeof(struct sMR));
}


/*----------------------------------------------------------------------------*/
static void *ActiveRemoteThread(void *args)
{
	int n;
	char buf[1024], command[20], ActiveRemote[11];
	char response[] = "HTTP/1.1 204 No Content\r\nDate: %s,%02d %s %4d %02d:%02d:%02d GMT\r\nDAAP-Server: iTunes/7.6.2 (Windows; N;)\r\nContent-Type: application/x-dmap-tagged\r\nContent-Length: 0\r\n\r\n";
	char *day[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	char *month[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sept", "Oct", "Nov", "Dec" };

	if (listen(glActiveRemoteSock, 5) < 0) {
		LOG_ERROR("Cannot listen", NULL);
		return NULL;
	}

	while (glMainRunning) {
		int sd, i;
		struct sockaddr cli_addr;
		socklen_t clilen = sizeof(cli_addr);
		struct sMR *Device = NULL;
		char *p;
		time_t now = time(NULL);
		struct tm gmt;

		sd = accept(glActiveRemoteSock, (struct sockaddr *) &cli_addr, &clilen);

		if (sd < 0) {
			if (glMainRunning) {
				LOG_ERROR("Accept error", NULL);
			}
			continue;
		}

		// receive data, all should be in a single receive, hopefully
		n = recv(sd, (void*) buf, 1024, 0);

		// a pretty basic reading of command
		buf[n] = '\0';
		strlwr(buf);
		p = strstr(buf, "active-remote:");
		sscanf(p, "active-remote:%s", ActiveRemote);
		p = strstr(buf, "/ctrl-int/1/");
		sscanf(p, "/ctrl-int/1/%s", command);

		for (i = 0; i < MAX_RENDERERS; i++) {
			if (glMRDevices[i].InUse && !strcmp(glMRDevices[i].ActiveRemote, ActiveRemote)) {
				Device = &glMRDevices[i];
				break;
			}
		}

		if (!Device) {
			LOG_ERROR("DACP from unknown player %s", buf);
			continue;
		}

		if (strstr(command, "pause")) sq_notify(Device->SqueezeHandle, Device, SQ_PAUSE, NULL, NULL);
		if (strstr(command, "play") || strstr(command, "playresume")) sq_notify(Device->SqueezeHandle, Device, SQ_PLAY, NULL, NULL);
		if (strstr(command, "playpause")) sq_notify(Device->SqueezeHandle, Device, SQ_PLAY_PAUSE, NULL, NULL);
		if (strstr(command, "stop")) sq_notify(Device->SqueezeHandle, Device, SQ_STOP, NULL, NULL);
		if (strstr(command, "mutetoggle")) sq_notify(Device->SqueezeHandle, Device, SQ_MUTE_TOGGLE, NULL, NULL);
		if (strstr(command, "nextitem")) sq_notify(Device->SqueezeHandle, Device, SQ_NEXT, NULL, NULL);
		if (strstr(command, "previtem")) sq_notify(Device->SqueezeHandle, Device, SQ_PREVIOUS, NULL, NULL);
		if (strstr(command, "volumeup")) sq_notify(Device->SqueezeHandle, Device, SQ_VOLUME, NULL, "up");
		if (strstr(command, "volumedown")) sq_notify(Device->SqueezeHandle, Device, SQ_PREVIOUS, NULL, "down");

		// send pre-made response
		gmt = *gmtime(&now);
		sprintf(buf, response, day[gmt.tm_wday], gmt.tm_mday, month[gmt.tm_mon],
								gmt.tm_year + 1900, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
		n = send(sd, buf, strlen(buf), 0);

		close(sd);
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
void StartActiveRemote(u32_t host)
{
	struct mdns_service *svc;
	struct sockaddr_in my_addr;
	socklen_t nlen = sizeof(struct sockaddr);
	int port;
	char buf[128];
	const char *txt[] = {
		"txtvers=1",
		"Ver=1",
		"DbId=63B5E5C0C201542E",
		"OSsi=0x1F5",
		NULL
	};

	if ((glActiveRemoteSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		LOG_ERROR("Cannot create ActiveRemote socket", NULL);
		return;
	}

	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_addr.s_addr = host;
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = 0;

	if (bind(glActiveRemoteSock, (struct sockaddr *) &my_addr, sizeof(my_addr)) < 0) {
		LOG_ERROR("Cannot bind ActiveRemote: %s", strerror(errno));
		return;
	}

	getsockname(glActiveRemoteSock, (struct sockaddr *) &my_addr, &nlen);
	port = ntohs(my_addr.sin_port);

	// start mDNS responder
	if ((gl_mDNSResponder = mdnsd_start(host)) == NULL) {
		LOG_ERROR("mdnsd responder start error", NULL);
	}

	mdnsd_set_hostname(gl_mDNSResponder, "airplay_bridge.local", host);
	sprintf(buf, "iTunes_Ctrl_%s", glDACPid);
	svc = mdnsd_register_svc(gl_mDNSResponder, buf, "_dacp._tcp.local", port, NULL, txt);
	mdns_service_destroy(svc);

	// start ActiveRemote answering thread
	pthread_create(&glActiveRemoteThread, NULL, ActiveRemoteThread, NULL);
}



/*----------------------------------------------------------------------------*/
void StopActiveRemote(void)
{
	if (glActiveRemoteSock != -1) {
#if WIN
		shutdown(glActiveRemoteSock, SD_BOTH);
#else
		shutdown(glActiveRemoteSock, SHUT_RDWR);
#endif
		close(glActiveRemoteSock);
	}
	pthread_join(glActiveRemoteThread, NULL);
	mdnsd_stop(gl_mDNSResponder);
}


/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	pthread_attr_t attr;

	if (glScanInterval) {
		if (glScanInterval < SCAN_INTERVAL) glScanInterval = SCAN_INTERVAL;
		if (glScanTimeout < SCAN_TIMEOUT) glScanTimeout = SCAN_TIMEOUT;
		if (glScanTimeout > glScanInterval - SCAN_TIMEOUT) glScanTimeout = glScanInterval - SCAN_TIMEOUT;
	}

	memset(&glMRDevices, 0, sizeof(glMRDevices));

	if (strstr(glInterface, "?")) get_local_hostname(glInterface, 16);

	// init mDNS
	gl_mDNSId = init_mDNS(false, inet_addr(glInterface));

	// Start the ActiveRemote server
	StartActiveRemote(inet_addr(glInterface));

	/* start the main thread */
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 64*1024);
	pthread_create(&glMainThread, &attr, &MainThread, NULL);
	pthread_attr_destroy(&attr);

	return true;
}


/*---------------------------------------------------------------------------*/
static bool Stop(void)
{
	pthread_join(glUpdateMRThread, NULL);

	LOG_DEBUG("flush renderers ...", NULL);
	FlushRaopDevices();

	// this forces an ongoing search to end
	LOG_DEBUG("terminate mDNS queries", NULL);
	close_mDNS(gl_mDNSId);

	// Stop ActiveRemote server
	LOG_DEBUG("terminate mDNS responder", NULL);
	StopActiveRemote();

	LOG_DEBUG("terminate main thread ...", NULL);
	pthread_join(glMainThread, NULL);

	return true;
}


/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	glMainRunning = false;

	if (!glGracefullShutdown) {
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}

	sq_end();
	Stop();
	exit(EXIT_SUCCESS);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	int i;

#define MAXCMDLINE 256
	char cmdline[MAXCMDLINE] = "";

	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < MAXCMDLINE); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("stxdfpi", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk"
#if defined(RESAMPLE)
						  "uR"
#endif
		  , opt)) {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 's':
			strcpy(glDeviceParam.server, optarg);
			break;
#if defined(RESAMPLE)
		case 'u':
		case 'R':
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				strcpy(glDeviceParam.resample_options, argv[optind++]);
				glDeviceParam.resample = true;
			} else {
				strcpy(glDeviceParam.resample_options, "");
				glDeviceParam.resample = false;
			}
			break;
#endif
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			glSaveConfigFile = optarg;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;

#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto"))	slimproto_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "raop"))    	raop_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))    slimmain_loglevel = new;				}
				else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		default:
			break;
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGPIPE)
	signal(SIGPIPE, SIG_IGN);
#endif
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting squeeze2raop version: %s\n", VERSION);

	if (!glConfigID) {
		LOG_ERROR("\n\n!!!!!!!!!!!!!!!!!! ERROR LOADING CONFIG FILE !!!!!!!!!!!!!!!!!!!!!\n", NULL);
	}

#if LINUX || FREEBSD
	if (glDaemonize && !glSaveConfigFile) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", getpid());
			fclose(pid_file);
		}
		else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	sq_init();

	if (!Start()) {
		LOG_ERROR("Cannot start", NULL);
		strcpy(resp, "exit");
	}

	if (glSaveConfigFile) {
		while (!glDiscovery) sleep(1);
		SaveConfig(glSaveConfigFile, glConfigID, true);
	}

	while (strcmp(resp, "exit") && !glSaveConfigFile) {

#if LINUX || FREEBSD
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else
			pause();
#else
		if (glInteractive)
			i = scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

	if (!strcmp(resp, "streamdbg"))	{
			char level[20];
			i = scanf("%s", level);
			stream_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "outputdbg"))	{
			char level[20];
			i = scanf("%s", level);
			output_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "slimprotodbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimproto_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "maindbg"))	{
			char level[20];
			i = scanf("%s", level);
			main_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "slimmainqdbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimmain_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "utildbg"))	{
			char level[20];
			i = scanf("%s", level);
			util_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "raopdbg"))	{
			char level[20];
			i = scanf("%s", level);
			raop_loglevel = debug2level(level);
		}
		 if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}
	}

	if (glConfigID) ixmlDocument_free(glConfigID);
	glMainRunning = false;
	LOG_INFO("stopping squeelite devices ...", NULL);
	sq_end();
	LOG_INFO("stopping Raop devices ...", NULL);
	Stop();
	LOG_INFO("all done", NULL);

	return true;
}




