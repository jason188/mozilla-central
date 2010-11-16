/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Radha Kulkarni(radha@netscape.com)
 *   Michal Novotny <michal.novotny@gmail.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nsWyciwyg.h"
#include "nsWyciwygChannel.h"
#include "nsIServiceManager.h"
#include "nsILoadGroup.h"
#include "nsIScriptSecurityManager.h"
#include "nsNetUtil.h"
#include "nsContentUtils.h"
#include "nsICacheService.h"
#include "nsICacheSession.h"
#include "nsIParser.h"
#include "nsThreadUtils.h"

// Must delete nsWyciwygChannel on main thread (URI destructor not thread
// safe), so Async IO events send event to Release channel on main thread
class nsWyciwygAsyncEvent : public nsRunnable {
public:
  nsWyciwygAsyncEvent(nsWyciwygChannel *aChannel) : mChannel(aChannel) {}

  ~nsWyciwygAsyncEvent() 
  {
    // NS_NewRunnableMethod addrefs channel and will release it on main
    // thread--which is the goal here--so target method is a no-op
    nsCOMPtr<nsIRunnable> event =
      NS_NewRunnableMethod(mChannel, &nsWyciwygChannel::MainReleaseNoOp);

    // ensure we free our ref before event gets released on main thread
    mChannel = 0;
    NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
  }
protected:
  nsRefPtr<nsWyciwygChannel> mChannel;
};

class nsWyciwygSetCharsetandSourceEvent : public nsWyciwygAsyncEvent {
public:
  nsWyciwygSetCharsetandSourceEvent(nsWyciwygChannel *aChannel)
    : nsWyciwygAsyncEvent(aChannel) {}

  NS_IMETHOD Run()
  {
    mChannel->SetCharsetAndSourceInternal();
    return NS_OK;
  }
};

class nsWyciwygWriteEvent : public nsWyciwygAsyncEvent {
public:
  nsWyciwygWriteEvent(nsWyciwygChannel *aChannel, const nsAString &aData, 
                      const nsACString &spec)
    : nsWyciwygAsyncEvent(aChannel), mData(aData), mSpec(spec) {}

  NS_IMETHOD Run()
  {
    mChannel->WriteToCacheEntryInternal(mData, mSpec);
    return NS_OK;
  }
private:
  nsString mData;
  nsCString mSpec;
};

class nsWyciwygCloseEvent : public nsWyciwygAsyncEvent {
public:
  nsWyciwygCloseEvent(nsWyciwygChannel *aChannel, nsresult aReason)
    : nsWyciwygAsyncEvent(aChannel), mReason(aReason) {}

  NS_IMETHOD Run()
  {
    mChannel->CloseCacheEntryInternal(mReason);
    return NS_OK;
  }
private:
  nsresult mReason;
};


// nsWyciwygChannel methods 
nsWyciwygChannel::nsWyciwygChannel()
  : mStatus(NS_OK),
    mIsPending(PR_FALSE),
    mCharsetAndSourceSet(PR_FALSE),
    mNeedToWriteCharset(PR_FALSE),
    mCharsetSource(kCharsetUninitialized),
    mContentLength(-1),
    mLoadFlags(LOAD_NORMAL)
{
}

nsWyciwygChannel::~nsWyciwygChannel() 
{
}

NS_IMPL_THREADSAFE_ISUPPORTS6(nsWyciwygChannel,
                              nsIChannel,
                              nsIRequest,
                              nsIStreamListener,
                              nsIRequestObserver,
                              nsICacheListener, 
                              nsIWyciwygChannel)

nsresult
nsWyciwygChannel::Init(nsIURI* uri)
{
  NS_ENSURE_ARG_POINTER(uri);

  nsresult rv;

  mURI = uri;
  mOriginalURI = uri;

  nsCOMPtr<nsICacheService> serv =
    do_GetService(NS_CACHESERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = serv->GetCacheIOTarget(getter_AddRefs(mCacheIOTarget));
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

///////////////////////////////////////////////////////////////////////////////
// nsIRequest methods:
///////////////////////////////////////////////////////////////////////////////

NS_IMETHODIMP
nsWyciwygChannel::GetName(nsACString &aName)
{
  return mURI->GetSpec(aName);
}
 
NS_IMETHODIMP
nsWyciwygChannel::IsPending(PRBool *aIsPending)
{
  *aIsPending = mIsPending;
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetStatus(nsresult *aStatus)
{
  if (NS_SUCCEEDED(mStatus) && mPump)
    mPump->GetStatus(aStatus);
  else
    *aStatus = mStatus;
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::Cancel(nsresult status)
{
  mStatus = status;
  if (mPump)
    mPump->Cancel(status);
  // else we're waiting for OnCacheEntryAvailable
  return NS_OK;
}
 
NS_IMETHODIMP
nsWyciwygChannel::Suspend()
{
  if (mPump)
    mPump->Suspend();
  // XXX else, we'll ignore this ... and that's probably bad!
  return NS_OK;
}
 
NS_IMETHODIMP
nsWyciwygChannel::Resume()
{
  if (mPump)
    mPump->Resume();
  // XXX else, we'll ignore this ... and that's probably bad!
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetLoadGroup(nsILoadGroup* *aLoadGroup)
{
  *aLoadGroup = mLoadGroup;
  NS_IF_ADDREF(*aLoadGroup);
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetLoadGroup(nsILoadGroup* aLoadGroup)
{
  mLoadGroup = aLoadGroup;
  NS_QueryNotificationCallbacks(mCallbacks,
                                mLoadGroup,
                                NS_GET_IID(nsIProgressEventSink),
                                getter_AddRefs(mProgressSink));
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetLoadFlags(PRUint32 aLoadFlags)
{
  mLoadFlags = aLoadFlags;
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetLoadFlags(PRUint32 * aLoadFlags)
{
  *aLoadFlags = mLoadFlags;
  return NS_OK;
}

////////////////////////////////////////////////////////////////////////////////
// nsIChannel methods:
///////////////////////////////////////////////////////////////////////////////

NS_IMETHODIMP
nsWyciwygChannel::GetOriginalURI(nsIURI* *aURI)
{
  *aURI = mOriginalURI;
  NS_ADDREF(*aURI);
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetOriginalURI(nsIURI* aURI)
{
  NS_ENSURE_ARG_POINTER(aURI);
  mOriginalURI = aURI;
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetURI(nsIURI* *aURI)
{
  *aURI = mURI;
  NS_IF_ADDREF(*aURI);
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetOwner(nsISupports **aOwner)
{
  NS_PRECONDITION(mOwner, "Must have a principal!");
  NS_ENSURE_STATE(mOwner);

  NS_ADDREF(*aOwner = mOwner);
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetOwner(nsISupports* aOwner)
{
  mOwner = aOwner;
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetNotificationCallbacks(nsIInterfaceRequestor* *aCallbacks)
{
  *aCallbacks = mCallbacks.get();
  NS_IF_ADDREF(*aCallbacks);
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetNotificationCallbacks(nsIInterfaceRequestor* aNotificationCallbacks)
{
  mCallbacks = aNotificationCallbacks;
  NS_QueryNotificationCallbacks(mCallbacks,
                                mLoadGroup,
                                NS_GET_IID(nsIProgressEventSink),
                                getter_AddRefs(mProgressSink));
  return NS_OK;
}

NS_IMETHODIMP 
nsWyciwygChannel::GetSecurityInfo(nsISupports * *aSecurityInfo)
{
  NS_IF_ADDREF(*aSecurityInfo = mSecurityInfo);

  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::GetContentType(nsACString &aContentType)
{
  aContentType.AssignLiteral(WYCIWYG_TYPE);
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetContentType(const nsACString &aContentType)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsWyciwygChannel::GetContentCharset(nsACString &aContentCharset)
{
  aContentCharset.Assign("UTF-16");
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetContentCharset(const nsACString &aContentCharset)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsWyciwygChannel::GetContentLength(PRInt32 *aContentLength)
{
  *aContentLength = mContentLength;
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetContentLength(PRInt32 aContentLength)
{
  mContentLength = aContentLength;

  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::Open(nsIInputStream ** aReturn)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsWyciwygChannel::AsyncOpen(nsIStreamListener *listener, nsISupports *ctx)
{
  LOG(("nsWyciwygChannel::AsyncOpen [this=%x]\n", this));

#ifndef MOZ_IPC
  // The only places creating wyciwyg: channels should be
  // HTMLDocument::OpenCommon and session history.  Both should be setting an
  // owner.
  NS_PRECONDITION(mOwner, "Must have a principal");
  NS_ENSURE_STATE(mOwner);
#endif

  NS_ENSURE_TRUE(!mIsPending, NS_ERROR_IN_PROGRESS);
  NS_ENSURE_ARG_POINTER(listener);

  nsCAutoString spec;
  mURI->GetSpec(spec);

  // open a cache entry for this channel...
  nsresult rv = OpenCacheEntry(spec, nsICache::ACCESS_READ);
  if (rv == NS_ERROR_CACHE_KEY_NOT_FOUND) {
    // Overwrite rv on purpose; if event dispatch fails we'll bail, and
    // otherwise we'll wait until the event fires before calling back.
    rv = NS_DispatchToCurrentThread(
            NS_NewRunnableMethod(this, &nsWyciwygChannel::NotifyListener));
  }

  if (NS_FAILED(rv)) {
    LOG(("nsWyciwygChannel::OpenCacheEntry failed [rv=%x]\n", rv));
    return rv;
  }

  mIsPending = PR_TRUE;
  mListener = listener;
  mListenerContext = ctx;

  if (mLoadGroup)
    mLoadGroup->AddRequest(this, nsnull);

  return NS_OK;
}

//////////////////////////////////////////////////////////////////////////////
// nsIWyciwygChannel
//////////////////////////////////////////////////////////////////////////////

NS_IMETHODIMP
nsWyciwygChannel::WriteToCacheEntry(const nsAString &aData)
{
  // URIs not thread-safe, so get spec now in case we need it
  nsCAutoString spec;
  nsresult rv = mURI->GetAsciiSpec(spec);
  if (NS_FAILED(rv)) 
    return rv;

  return mCacheIOTarget->Dispatch(new nsWyciwygWriteEvent(this, aData, spec),
                                  NS_DISPATCH_NORMAL);
}

nsresult
nsWyciwygChannel::WriteToCacheEntryInternal(const nsAString &aData, const nsACString& spec)
{
  NS_ASSERTION(IsOnCacheIOThread(), "wrong thread");

  nsresult rv;

  if (!mCacheEntry) {
    rv = OpenCacheEntry(spec, nsICache::ACCESS_WRITE);
    if (NS_FAILED(rv)) return rv;
  }

  if (mSecurityInfo) {
    mCacheEntry->SetSecurityInfo(mSecurityInfo);
  }

  if (mNeedToWriteCharset) {
    WriteCharsetAndSourceToCache(mCharsetSource, mCharset);
    mNeedToWriteCharset = PR_FALSE;
  }
  
  PRUint32 out;
  if (!mCacheOutputStream) {
    // Get the outputstream from the cache entry.
    rv = mCacheEntry->OpenOutputStream(0, getter_AddRefs(mCacheOutputStream));    
    if (NS_FAILED(rv)) return rv;

    // Write out a Byte Order Mark, so that we'll know if the data is
    // BE or LE when we go to read it.
    PRUnichar bom = 0xFEFF;
    rv = mCacheOutputStream->Write((char *)&bom, sizeof(bom), &out);
    if (NS_FAILED(rv)) return rv;
  }

  return mCacheOutputStream->Write((char *)PromiseFlatString(aData).get(),
                                   aData.Length() * sizeof(PRUnichar), &out);
}


NS_IMETHODIMP
nsWyciwygChannel::CloseCacheEntry(nsresult reason)
{
  return mCacheIOTarget->Dispatch(new nsWyciwygCloseEvent(this, reason),
                                  NS_DISPATCH_NORMAL);
}

nsresult
nsWyciwygChannel::CloseCacheEntryInternal(nsresult reason)
{
  NS_ASSERTION(IsOnCacheIOThread(), "wrong thread");

  if (mCacheEntry) {
    LOG(("nsWyciwygChannel::CloseCacheEntryInternal [this=%x ]", this));
    mCacheOutputStream = 0;
    mCacheInputStream = 0;

    if (NS_FAILED(reason))
      mCacheEntry->Doom();

    mCacheEntry = 0;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetSecurityInfo(nsISupports *aSecurityInfo)
{
  mSecurityInfo = aSecurityInfo;

  return NS_OK;
}

NS_IMETHODIMP
nsWyciwygChannel::SetCharsetAndSource(PRInt32 aSource,
                                      const nsACString& aCharset)
{
  NS_ENSURE_ARG(!aCharset.IsEmpty());

  mCharsetAndSourceSet = PR_TRUE;
  mCharset = aCharset;
  mCharsetSource = aSource;

  return mCacheIOTarget->Dispatch(new nsWyciwygSetCharsetandSourceEvent(this),
                                  NS_DISPATCH_NORMAL);
}

void
nsWyciwygChannel::SetCharsetAndSourceInternal()
{
  NS_ASSERTION(IsOnCacheIOThread(), "wrong thread");

  if (mCacheEntry) {
    WriteCharsetAndSourceToCache(mCharsetSource, mCharset);
  } else {
    mNeedToWriteCharset = PR_TRUE;
  }
}

NS_IMETHODIMP
nsWyciwygChannel::GetCharsetAndSource(PRInt32* aSource, nsACString& aCharset)
{
  if (mCharsetAndSourceSet) {
    *aSource = mCharsetSource;
    aCharset = mCharset;
    return NS_OK;
  }

  if (!mCacheEntry) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsXPIDLCString data;
  mCacheEntry->GetMetaDataElement("charset", getter_Copies(data));

  if (data.IsEmpty()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsXPIDLCString sourceStr;
  mCacheEntry->GetMetaDataElement("charset-source", getter_Copies(sourceStr));

  PRInt32 source;
  // XXXbz ToInteger takes an PRInt32* but outputs an nsresult in it... :(
  PRInt32 err;
  source = sourceStr.ToInteger(&err);
  if (NS_FAILED(err) || source == 0) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  *aSource = source;
  aCharset = data;
  return NS_OK;
}

//////////////////////////////////////////////////////////////////////////////
// nsICachelistener
//////////////////////////////////////////////////////////////////////////////
NS_IMETHODIMP
nsWyciwygChannel::OnCacheEntryAvailable(nsICacheEntryDescriptor * aCacheEntry, nsCacheAccessMode aMode, nsresult aStatus)
{
  LOG(("nsWyciwygChannel::OnCacheEntryAvailable [this=%x entry=%x "
       "access=%x status=%x]\n", this, aCacheEntry, aMode, aStatus));

  // if the channel's already fired onStopRequest, 
  // then we should ignore this event.
  if (!mIsPending)
    return NS_OK;

  // otherwise, we have to handle this event.
  if (NS_SUCCEEDED(aStatus))
    mCacheEntry = aCacheEntry;
  else if (NS_SUCCEEDED(mStatus))
    mStatus = aStatus;

  nsresult rv;
  if (NS_FAILED(mStatus)) {
    LOG(("channel was canceled [this=%x status=%x]\n", this, mStatus));
    rv = mStatus;
  }
  else { // advance to the next state...
    rv = ReadFromCache();
  }

  // a failure from Connect means that we have to abort the channel.
  if (NS_FAILED(rv)) {
    CloseCacheEntry(rv);

    NotifyListener();
  }

  return NS_OK;
}

//-----------------------------------------------------------------------------
// nsWyciwygChannel::nsIStreamListener
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsWyciwygChannel::OnDataAvailable(nsIRequest *request, nsISupports *ctx,
                                  nsIInputStream *input,
                                  PRUint32 offset, PRUint32 count)
{
  LOG(("nsWyciwygChannel::OnDataAvailable [this=%x request=%x offset=%u count=%u]\n",
      this, request, offset, count));

  nsresult rv;
  
  rv = mListener->OnDataAvailable(this, mListenerContext, input, offset, count);

  // XXX handle 64-bit stuff for real
  if (mProgressSink && NS_SUCCEEDED(rv) && !(mLoadFlags & LOAD_BACKGROUND))
    mProgressSink->OnProgress(this, nsnull, PRUint64(offset + count),
                              PRUint64(mContentLength));

  return rv; // let the pump cancel on failure
}

//////////////////////////////////////////////////////////////////////////////
// nsIRequestObserver
//////////////////////////////////////////////////////////////////////////////

NS_IMETHODIMP
nsWyciwygChannel::OnStartRequest(nsIRequest *request, nsISupports *ctx)
{
  LOG(("nsWyciwygChannel::OnStartRequest [this=%x request=%x\n",
      this, request));

  return mListener->OnStartRequest(this, mListenerContext);
}


NS_IMETHODIMP
nsWyciwygChannel::OnStopRequest(nsIRequest *request, nsISupports *ctx, nsresult status)
{
  LOG(("nsWyciwygChannel::OnStopRequest [this=%x request=%x status=%d\n",
      this, request, status));

  if (NS_SUCCEEDED(mStatus))
    mStatus = status;

  mListener->OnStopRequest(this, mListenerContext, mStatus);
  mListener = 0;
  mListenerContext = 0;

  if (mLoadGroup)
    mLoadGroup->RemoveRequest(this, nsnull, mStatus);

  CloseCacheEntry(mStatus);
  mPump = 0;
  mIsPending = PR_FALSE;

  // Drop notification callbacks to prevent cycles.
  mCallbacks = 0;
  mProgressSink = 0;

  return NS_OK;
}

//////////////////////////////////////////////////////////////////////////////
// Helper functions
//////////////////////////////////////////////////////////////////////////////

nsresult
nsWyciwygChannel::OpenCacheEntry(const nsACString & aCacheKey,
                                 nsCacheAccessMode aAccessMode)
{
  nsresult rv = NS_ERROR_FAILURE;
  // Get cache service
  nsCOMPtr<nsICacheService> cacheService =
    do_GetService(NS_CACHESERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // honor security settings
  nsCacheStoragePolicy storagePolicy;
  if (mLoadFlags & INHIBIT_PERSISTENT_CACHING)
    storagePolicy = nsICache::STORE_IN_MEMORY;
  else
    storagePolicy = nsICache::STORE_ANYWHERE;

  nsCOMPtr<nsICacheSession> cacheSession;
  // Open a stream based cache session.
  rv = cacheService->CreateSession("wyciwyg", storagePolicy, PR_TRUE,
                                   getter_AddRefs(cacheSession));
  if (!cacheSession) 
    return NS_ERROR_FAILURE;

  if (aAccessMode == nsICache::ACCESS_WRITE)
    rv = cacheSession->OpenCacheEntry(aCacheKey, aAccessMode, PR_FALSE,
                                      getter_AddRefs(mCacheEntry));
  else
    rv = cacheSession->AsyncOpenCacheEntry(aCacheKey, aAccessMode, this);

  return rv;
}

nsresult
nsWyciwygChannel::ReadFromCache()
{
  LOG(("nsWyciwygChannel::ReadFromCache [this=%x] ", this));

  NS_ENSURE_TRUE(mCacheEntry, NS_ERROR_FAILURE);
  nsresult rv;

  // Get the stored security info
  mCacheEntry->GetSecurityInfo(getter_AddRefs(mSecurityInfo));

  // Get a transport to the cached data...
  rv = mCacheEntry->OpenInputStream(0, getter_AddRefs(mCacheInputStream));
  if (NS_FAILED(rv))
    return rv;
  NS_ENSURE_TRUE(mCacheInputStream, NS_ERROR_UNEXPECTED);

  rv = NS_NewInputStreamPump(getter_AddRefs(mPump), mCacheInputStream);
  if (NS_FAILED(rv)) return rv;

  // Pump the cache data downstream
  return mPump->AsyncRead(this, nsnull);
}

void
nsWyciwygChannel::WriteCharsetAndSourceToCache(PRInt32 aSource,
                                               const nsCString& aCharset)
{
  NS_ASSERTION(IsOnCacheIOThread(), "wrong thread");
  NS_PRECONDITION(mCacheEntry, "Better have cache entry!");
  
  mCacheEntry->SetMetaDataElement("charset", aCharset.get());

  nsCAutoString source;
  source.AppendInt(aSource);
  mCacheEntry->SetMetaDataElement("charset-source", source.get());
}

void
nsWyciwygChannel::NotifyListener()
{    
  if (mListener) {
    mListener->OnStartRequest(this, mListenerContext);
    mListener->OnStopRequest(this, mListenerContext, mStatus);
    mListener = 0;
    mListenerContext = 0;
  }

  mIsPending = PR_FALSE;

  // Remove ourselves from the load group.
  if (mLoadGroup) {
    mLoadGroup->RemoveRequest(this, nsnull, mStatus);
  }
}

PRBool
nsWyciwygChannel::IsOnCacheIOThread()
{
  PRBool correctThread;
  mCacheIOTarget->IsOnCurrentThread(&correctThread);
  return correctThread;
}

// vim: ts=2 sw=2