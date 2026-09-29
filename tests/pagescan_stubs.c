/*
 * Link stubs for the PageScan lifecycle harness.
 * ReadNonFixedData is scripted. Everything else is a no-op so a test can
 * reach PageScan without libusb.
 */
#include <stdarg.h>
#include <string.h>

#include "brother.h"
#include "brother_devaccs.h"
#include "brother_mfccmd.h"
#include "brother_cmatch.h"
#include "brother_log.h"
#include "brother_brscan4.h"
#include "brother_modelinf.h"
#include "brother_color.h"
#include "brother_netdev.h"

int ChangeEndpoint[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
WORD gwInBuffSize = 32768;
BOOL bTxCancelCmd = FALSE;
TDevice *g_pdev = NULL;

struct RxChunk {
	const unsigned char *bytes;
	int len;
};

static struct RxChunk g_chunks[8];
static int g_chunk_count;
static int g_chunk_index;

void brscan_test_reset_reads(void)
{
	g_chunk_count = 0;
	g_chunk_index = 0;
}

void brscan_test_push_read(const unsigned char *bytes, int len)
{
	if (g_chunk_count < 8) {
		g_chunks[g_chunk_count].bytes = bytes;
		g_chunks[g_chunk_count].len = len;
		g_chunk_count++;
	}
}

int brscan_test_reads_consumed(void)
{
	return g_chunk_index;
}

int ReadNonFixedData(usb_dev_handle *hScanner, LPSTR lpBuffer, WORD wReadSize,
		     DWORD dwTimeOut, int seriesNo)
{
	struct RxChunk *chunk;
	int n;

	(void)hScanner;
	(void)dwTimeOut;
	(void)seriesNo;
	if (g_chunk_index >= g_chunk_count)
		return 0;
	chunk = &g_chunks[g_chunk_index++];
	n = chunk->len;
	if (n > (int)wReadSize)
		n = (int)wReadSize;
	if (n > 0 && lpBuffer != NULL)
		memcpy(lpBuffer, chunk->bytes, (size_t)n);
	return n;
}

#pragma push_macro("usb_dev_handle")
#undef usb_dev_handle

int usb_bulk_read(usb_dev_handle *dev, int ep, char *buf, int size, int timeout)
{
	(void)dev;
	(void)ep;
	(void)buf;
	(void)size;
	(void)timeout;
	return 0;
}

int usb_claim_interface(usb_dev_handle *dev, int iface)
{
	(void)dev;
	(void)iface;
	return 0;
}

int usb_close(usb_dev_handle *dev)
{
	(void)dev;
	return 0;
}

usb_dev_handle *usb_open(struct usb_device *dev)
{
	(void)dev;
	return NULL;
}

#pragma pop_macro("usb_dev_handle")

int usb_set_configuration_or_reset_toggle(Brother_Scanner *scanner, int configuration)
{
	(void)scanner;
	(void)configuration;
	return 0;
}

int OpenDevice(usb_dev_handle *hScanner, int seriesNo)
{
	(void)hScanner;
	(void)seriesNo;
	return 1;
}

void CloseDevice(usb_dev_handle *hScanner)
{
	(void)hScanner;
}

int WriteDeviceCommand(usb_dev_handle *hScanner, LPSTR lpTxBuffer, int nWriteSize, int seriesNo)
{
	(void)hScanner;
	(void)lpTxBuffer;
	(void)nWriteSize;
	(void)seriesNo;
	return 1;
}

void SendCancelCommand(usb_dev_handle *hScanner, int seriesNo)
{
	(void)hScanner;
	(void)seriesNo;
}

HANDLE AllocReceiveBuffer(DWORD dwBuffSize)
{
	(void)dwBuffSize;
	return NULL;
}

void FreeReceiveBuffer(void)
{
}

int MakeupScanStartCmd(Brother_Scanner *scanner, LPSTR lpszCmdStr)
{
	(void)scanner;
	if (lpszCmdStr)
		lpszCmdStr[0] = '\0';
	return 0;
}

BOOL QueryScannerInfo(Brother_Scanner *scanner)
{
	(void)scanner;
	return 1;
}

void InitColorMatchingFunc(Brother_Scanner *scanner, WORD nColorType, int nRgbDataType)
{
	(void)scanner;
	(void)nColorType;
	(void)nRgbDataType;
}

void ExecColorMatchingFunc(Brother_Scanner *scanner, LPBYTE lpRgbData, long lRgbDataLen, long lLineCount)
{
	(void)scanner;
	(void)lpRgbData;
	(void)lRgbDataLen;
	(void)lLineCount;
}

HANDLE SetupGrayAdjust(Brother_Scanner *scanner)
{
	(void)scanner;
	return NULL;
}

void WriteLog(LPSTR first, ...)
{
	(void)first;
}

void WriteLogScanCmd(LPSTR lpszId, LPSTR lpszCmd)
{
	(void)lpszId;
	(void)lpszCmd;
}

void brother_color_cleanup(void)
{
}

int brscan4_is_boundary_status(unsigned char header)
{
	(void)header;
	return 0;
}

int brscan4_status_at_frame_boundary(const unsigned char *buf, unsigned int len)
{
	(void)buf;
	(void)len;
	return 0;
}

void brscan4_cache_reset(Brscan4ReadCache *cache)
{
	if (cache)
		cache->len = 0;
}

int brscan4_read_next_record(Brscan4ReadCache *cache, brscan4_read_fn read_fn,
			     void *read_ctx, unsigned char *dst, int maxlen)
{
	(void)cache;
	(void)read_fn;
	(void)read_ctx;
	(void)dst;
	(void)maxlen;
	return 0;
}

int read_device_net(br_net_dev_handle h_dev, char *buffer, int size,
		   int *preadsize, struct timeval *ptimeout)
{
	(void)h_dev;
	(void)buffer;
	(void)size;
	(void)ptimeout;
	if (preadsize)
		*preadsize = 0;
	return 0;
}

void *get_p_model_info_by_index(int index)
{
	(void)index;
	return NULL;
}
