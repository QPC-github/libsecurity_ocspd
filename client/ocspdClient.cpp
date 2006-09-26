/*
 * Copyright (c) 2002,2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/* 
 * ocspdClient.cpp - Client interface to OCSP helper daemon
 */
 
#include "ocspdClient.h"
#include "ocspdTypes.h"
#include "ocspdDebug.h"
#include <Security/cssmapple.h>
#include <security_utilities/threading.h>
#include <security_utilities/mach++.h>
#include <security_utilities/unix++.h>
#include <security_ocspd/ocspd.h>			/* MIG interface */
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>

class ocspdGlobals
{
public:
	ocspdGlobals();
	~ocspdGlobals();
	mach_port_t serverPort();
private:
	UnixPlusPlus::StaticForkMonitor mForkMonitor;
	MachPlusPlus::Port mServerPort;
	Mutex mLock;
};

ocspdGlobals::ocspdGlobals()
	: mServerPort(0)
{
	/* nothing here, the real work is done in serverPort() */
}

ocspdGlobals::~ocspdGlobals()
{
	/* I don't believe this should ever execute */
}

mach_port_t ocspdGlobals::serverPort()
{
	StLock<Mutex> _(mLock);
	
	// Guard against fork-without-exec. If we are the child of a fork
	// (that has not exec'ed), our apparent connection to SecurityServer
	// is just a mirage, and we better reset it.
	mach_port_t rtnPort = mServerPort.port();
	if (mForkMonitor()) {
		rtnPort = 0;
	}
	if(rtnPort != 0) {
		return rtnPort;
	}
	
	char *serverName = NULL;
	#ifndef	NDEBUG
	serverName = getenv(OCSPD_BOOTSTRAP_ENV);
	#endif
	if(serverName == NULL) {
		serverName = OCSPD_BOOTSTRAP_NAME;
	}
	try {
		mServerPort = MachPlusPlus::Bootstrap().lookup(serverName);
	}
	catch(...) {
		ocspdErrorLog("ocspdGlobals: error contacting server\n");
		throw;
	}
	return mServerPort;
}

static ModuleNexus<ocspdGlobals> OcspdGlobals;

/* 
 * Perform network fetch of an OCSP response. Result is not verified in any 
 * way.
 */
CSSM_RETURN ocspdFetch(
	Allocator			&alloc,
	const CSSM_DATA		&ocspdReq,		// DER-encoded SecAsn1OCSPDRequests
	CSSM_DATA			&ocspdResp)		// DER-encoded kSecAsn1OCSPDReplies
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	unsigned char *rtnData = NULL;
	unsigned rtnLen = 0;
	
	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdFetch: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}

	krtn = ocsp_client_ocspdFetch(serverPort, ocspdReq.Data, ocspdReq.Length,
		(void **)&rtnData, &rtnLen);
	if(krtn) {
		ocspdErrorLog("ocspdFetch: RPC returned %d\n", krtn);
		return CSSMERR_APPLETP_OCSP_UNAVAILABLE;
	}
	if((rtnData == NULL) || (rtnLen == 0)) {
		ocspdErrorLog("ocspdFetch: RPC returned NULL data\n");
		return CSSMERR_APPLETP_OCSP_UNAVAILABLE;
	}
	ocspdResp.Data = (uint8 *)alloc.malloc(rtnLen);
	ocspdResp.Length = rtnLen;
	memmove(ocspdResp.Data, rtnData, rtnLen);
	mig_deallocate((vm_address_t)rtnData, rtnLen);
	return CSSM_OK;
}

/* 
 * Flush all responses associated with specifed CertID from cache. 
 */
CSSM_RETURN ocspdCacheFlush(
	const CSSM_DATA		&certID)
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	
	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdCacheFlush: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	krtn = ocsp_client_ocspdCacheFlush(serverPort, certID.Data, certID.Length);
	if(krtn) {
		ocspdErrorLog("ocspdCacheFlush: RPC returned %d\n", krtn);
		return CSSMERR_APPLETP_OCSP_UNAVAILABLE;
	}
	return CSSM_OK;
}

/* 
 * Flush stale entries from cache. 
 */
CSSM_RETURN ocspdCacheFlushStale()
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	
	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdCacheFlush: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	krtn = ocsp_client_ocspdCacheFlushStale(serverPort);
	if(krtn) {
		ocspdErrorLog("ocsp_client_ocspdCacheFlushStale: RPC returned %d\n", krtn);
		return (CSSM_RETURN)krtn;
	}
	return CSSM_OK;
}

/* 
 * fetch a certificate from the net. 
 */
CSSM_RETURN ocspdCertFetch(
	Allocator			&alloc,
	const CSSM_DATA		&certURL,
	CSSM_DATA			&certData)		// mallocd via alloc and RETURNED
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	unsigned char *rtnData = NULL;
	unsigned rtnLen = 0;
	
	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdCertFetch: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	
	krtn = ocsp_client_certFetch(serverPort, certURL.Data, certURL.Length,
		(void **)&rtnData, &rtnLen);
	if(krtn) {
		ocspdErrorLog("ocspdCertFetch: RPC returned %d\n", krtn);
		return CSSMERR_APPLETP_NETWORK_FAILURE;
	}
	
	if((rtnData == NULL) || (rtnLen == 0)) {
		ocspdErrorLog("ocspdCertFetch: RPC returned NULL data\n");
		return CSSMERR_APPLETP_CERT_NOT_FOUND_FROM_ISSUER;
	}
	certData.Data = (uint8 *)alloc.malloc(rtnLen);
	certData.Length = rtnLen;
	memmove(certData.Data, rtnData, rtnLen);
	mig_deallocate((vm_address_t)rtnData, rtnLen);
	return CSSM_OK;
}

/*
 * Fetch a CRL from net with optional cache lookup and store.
 * verifyTime only used for cache lookup. 
 */
CSSM_RETURN ocspdCRLFetch(
	Allocator			&alloc,
	const CSSM_DATA		&crlURL,
	const CSSM_DATA		*crlIssuer,		// optional
	bool				cacheReadEnable,
	bool				cacheWriteEnable,
	CSSM_TIMESTRING 	verifyTime,
	CSSM_DATA			&crlData)		// mallocd via alloc and RETURNED
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	unsigned char *rtnData = NULL;
	unsigned rtnLen = 0;
	
	if(verifyTime == NULL) {
		ocspdErrorLog("ocspdCRLFetch: verifyTime NOT OPTIONAL\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdCRLFetch: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	
	krtn = ocsp_client_crlFetch(serverPort, crlURL.Data, crlURL.Length,
		crlIssuer ? crlIssuer->Data : NULL, crlIssuer ? crlIssuer->Length : 0,
		cacheReadEnable, cacheWriteEnable,
		verifyTime, strlen(verifyTime),
		(void **)&rtnData, &rtnLen);
	if(krtn) {
		ocspdErrorLog("ocspdCRLFetch: RPC returned %d\n", krtn);
		return CSSMERR_APPLETP_NETWORK_FAILURE;
	}
	
	if((rtnData == NULL) || (rtnLen == 0)) {
		ocspdErrorLog("ocspdCRLFetch: RPC returned NULL data\n");
		return CSSMERR_APPLETP_CRL_NOT_FOUND;
	}
	crlData.Data = (uint8 *)alloc.malloc(rtnLen);
	crlData.Length = rtnLen;
	memmove(crlData.Data, rtnData, rtnLen);
	mig_deallocate((vm_address_t)rtnData, rtnLen);
	return CSSM_OK;
}

/* 
 * Refresh the CRL cache. 
 */
CSSM_RETURN ocspdCRLRefresh(
	unsigned	staleDays,
	unsigned	expireOverlapSeconds,
	bool		purgeAll,
	bool		fullCryptoVerify)
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdCRLRefresh: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	
	krtn = ocsp_client_crlRefresh(serverPort, staleDays, expireOverlapSeconds,
		purgeAll, fullCryptoVerify);
	if(krtn) {
		ocspdErrorLog("ocspdCRLRefresh: RPC returned %d\n", krtn);
		return CSSMERR_APPLETP_NETWORK_FAILURE;
	}
	
	return CSSM_OK;
}

/* 
 * Flush all CRLs obtained from specified URL from cache. Called by client when 
 * *it* detects a bad CRL. 
 */
CSSM_RETURN ocspdCRLFlush(
	const CSSM_DATA		&crlURL)
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;

	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdCRLFlush: OCSPD server error\n");
		return CSSMERR_TP_INTERNAL_ERROR;
	}
	
	krtn = ocsp_client_crlFlush(serverPort, crlURL.Data, crlURL.Length);
	if(krtn) {
		ocspdErrorLog("ocspdCRLFlush: RPC returned %d\n", krtn);
		return CSSMERR_APPLETP_NETWORK_FAILURE;
	}
	return CSSM_OK;
}

/*
 * Obtain TrustSettings. 
 */
OSStatus ocspdTrustSettingsRead(
	Allocator				&alloc,
	SecTrustSettingsDomain 	domain,
	CSSM_DATA				&trustSettings)		// mallocd via alloc and RETURNED
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	unsigned char *rtnData = NULL;
	unsigned rtnLen = 0;
	OSStatus ortn;

	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdTrustSettingsRead: OCSPD server error\n");
		return internalComponentErr;
	}
	
	krtn = ocsp_client_trustSettingsRead(serverPort, domain,
		(void **)&rtnData, &rtnLen, &ortn);
	if(krtn) {
		ocspdErrorLog("ocspdTrustSettingsRead: RPC returned %d\n", krtn);
		return errSecNotAvailable;
	}
	if(ortn) {
		/* e.g., errSecNoUserTrustRecord */
		return ortn;
	}
	if((rtnData == NULL) || (rtnLen == 0)) {
		ocspdErrorLog("ocspdTrustSettingsRead: RPC returned NULL data\n");
		return errSecItemNotFound;
	}
	trustSettings.Data = (uint8 *)alloc.malloc(rtnLen);
	trustSettings.Length = rtnLen;
	memmove(trustSettings.Data, rtnData, rtnLen);
	mig_deallocate((vm_address_t)rtnData, rtnLen);
	return noErr;
}

/*
 * Write TrustSettings to disk. Results in authentication dialog.
 */
OSStatus ocspdTrustSettingsWrite(
	SecTrustSettingsDomain 	domain,
	const CSSM_DATA			&authBlob,
	const CSSM_DATA			&trustSettings)
{
	mach_port_t serverPort = 0;
	kern_return_t krtn;
	OSStatus ortn;

	try {
		serverPort = OcspdGlobals().serverPort();
	} 
	catch(...) {
		ocspdErrorLog("ocspdTrustSettingsWrite: OCSPD server error\n");
		return internalComponentErr;
	}

	krtn = ocsp_client_trustSettingsWrite(serverPort, domain,
		authBlob.Data, authBlob.Length,
		trustSettings.Data, trustSettings.Length,
		&ortn);
	if(krtn) {
		ocspdErrorLog("ocspdTrustSettingsWrite: RPC returned %d\n", krtn);
		return internalComponentErr;
	}
	return ortn;
}
