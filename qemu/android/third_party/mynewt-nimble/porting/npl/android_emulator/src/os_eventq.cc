// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


// This contains abstractions to run NimBLE against rootcanal (emulated
// bluetooth chip)

#include <stdio.h>
#include <assert.h>                // for assert
#include <string.h>                // for memset, NULL

#include "nimble/nimble_npl.h"     // for ble_npl_event_fn, ble_npl_event_ge...
#include "nimble/nimble_npl_os.h"  // for BLE_NPL_TIME_FOREVER
#include "nimble/os_types.h"       // for ble_npl_event, ble_npl_eventq, ble...
#include "wqueue.h"                // for wqueue
#include <vector>

extern "C" {

typedef wqueue<ble_npl_event *> wqueue_t;

class DflEventQ {
 public:
    DflEventQ() {
        mEventQueue.q = &mQueue;
    }

    ~DflEventQ() {
        printf("Bye yall!");
    }

    ble_npl_eventq mEventQueue;
    wqueue_t mQueue;
};


static DflEventQ g_event_queue;
static std::vector<wqueue_t> g_queue;

struct ble_npl_eventq *
ble_npl_eventq_dflt_get(void)
{
    return &g_event_queue.mEventQueue;
}

void
ble_npl_eventq_init(struct ble_npl_eventq *evq)
{
    evq->q = new wqueue_t();
}

bool
ble_npl_eventq_is_empty(struct ble_npl_eventq *evq)
{
    wqueue_t *q = static_cast<wqueue_t *>(evq->q);

    if (q->size()) {
        return 1;
    } else {
        return 0;
    }
}

int
ble_npl_eventq_inited(const struct ble_npl_eventq *evq)
{
    return (evq->q != NULL);
}

void
ble_npl_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
    wqueue_t *q = static_cast<wqueue_t *>(evq->q);

    if (ev->ev_queued) {
        return;
    }

    ev->ev_queued = 1;
    q->put(ev);
}

struct ble_npl_event *ble_npl_eventq_get(struct ble_npl_eventq *evq,
                                         ble_npl_time_t tmo)
{
    struct ble_npl_event *ev;
    wqueue_t *q = static_cast<wqueue_t *>(evq->q);

    ev = q->get(tmo);

    if (ev) {
        ev->ev_queued = 0;
    }

    return ev;
}

void
ble_npl_eventq_run(struct ble_npl_eventq *evq)
{
    struct ble_npl_event *ev;

    ev = ble_npl_eventq_get(evq, BLE_NPL_TIME_FOREVER);
    ble_npl_event_run(ev);
}


// ========================================================================
//                         Event Implementation
// ========================================================================

void
ble_npl_event_init(struct ble_npl_event *ev, ble_npl_event_fn *fn,
                   void *arg)
{
    memset(ev, 0, sizeof(*ev));
    ev->ev_cb = fn;
    ev->ev_arg = arg;
}

bool
ble_npl_event_is_queued(struct ble_npl_event *ev)
{
    return ev->ev_queued;
}

void *
ble_npl_event_get_arg(struct ble_npl_event *ev)
{
    return ev->ev_arg;
}

void
ble_npl_event_set_arg(struct ble_npl_event *ev, void *arg)
{
    ev->ev_arg = arg;
}

void
ble_npl_event_run(struct ble_npl_event *ev)
{
    assert(ev->ev_cb != NULL);

    ev->ev_cb(ev);
}

void
ble_npl_eventq_remove(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
    wqueue_t *q = static_cast<wqueue_t *>(evq->q);

    if (!ev->ev_queued) {
        return;
    }

    ev->ev_queued = 0;
    q->remove(ev);
}

}
