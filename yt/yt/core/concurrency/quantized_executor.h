#pragma once

#include "public.h"

#include <yt/yt/core/actions/future.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

struct ICallbackProvider
    : public TRefCounted
{
    virtual TCallback<void()> ExtractCallback() = 0;
};

DEFINE_REFCOUNTED_TYPE(ICallbackProvider)

////////////////////////////////////////////////////////////////////////////////

//! A device that executes provided callbacks in multiple threads
//! during some quanta of time. At another time worker threads are
//! blocked.
struct IQuantizedExecutor
    : public TRefCounted
{
    virtual void Initialize(TCallback<void()> workerInitializer = {}) = 0;

    //! Starts new quantum of time, returns a future that becomes set
    //! when quantum ends.
    /*!
     *  Quantum ends when either #timeout is reached or there are no more
     *  enqueued callbacks in underlying threads.
     *  Quantum completion is implemented via suspention of underlying
     *  suspendable action queue. Timeout corresponds to immediate suspension
     *  and extracted null callback from callback provider corresponds to
     *  non-immediate suspension. Cf. ISuspendableActionQueue::Suspend.
     */
    virtual TFuture<void> Run(TDuration timeout) = 0;

    //! Updates number of workers.
    virtual void Reconfigure(int workerCount) = 0;
};

DEFINE_REFCOUNTED_TYPE(IQuantizedExecutor)

////////////////////////////////////////////////////////////////////////////////

IQuantizedExecutorPtr CreateQuantizedExecutor(
    TString name,
    ICallbackProviderPtr callbackProvider,
    int workerCount);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
