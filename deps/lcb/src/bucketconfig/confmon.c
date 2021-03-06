/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "internal.h"
#include "clconfig.h"
#define LOGARGS(mon, lvlbase) mon->settings, "confmon", LCB_LOG_##lvlbase, __FILE__, __LINE__
#define LOG(mon, lvlbase, msg) lcb_log(mon->settings, "confmon", LCB_LOG_##lvlbase, __FILE__, __LINE__, msg)

static void async_stop(void *);
static void async_start(void *);
static int do_next_provider(lcb_confmon *mon);
static void invoke_listeners(lcb_confmon *mon,
                             clconfig_event_t event,
                             clconfig_info *info);

#define IS_REFRESHING(mon) (mon)->state & CONFMON_S_ACTIVE

static clconfig_provider *next_active(lcb_confmon *mon, clconfig_provider *cur)
{
    if (!LCB_LIST_HAS_NEXT((lcb_list_t *)&mon->active_providers, &cur->ll)) {
        return NULL;
    }
    return LCB_LIST_ITEM(cur->ll.next, clconfig_provider, ll);
}
static clconfig_provider *first_active(lcb_confmon *mon)
{
    if (LCB_LIST_IS_EMPTY((lcb_list_t *)&mon->active_providers)) {
        return NULL;
    }
    return LCB_LIST_ITEM(mon->active_providers.next, clconfig_provider, ll);
}

static const char *
provider_string(clconfig_method_t type) {
    if (type == LCB_CLCONFIG_HTTP) { return "HTTP"; }
    if (type == LCB_CLCONFIG_CCCP) { return "CCCP"; }
    if (type == LCB_CLCONFIG_FILE) { return "FILE"; }
    return "";
}

lcb_confmon* lcb_confmon_create(lcb_settings *settings, lcbio_pTABLE iot)
{
    int ii;
    lcb_confmon * mon = calloc(1, sizeof(*mon));
    mon->settings = settings;
    mon->iot = iot;
    lcb_list_init(&mon->listeners);
    lcb_clist_init(&mon->active_providers);
    lcbio_table_ref(mon->iot);
    lcb_settings_ref(mon->settings);

    mon->all_providers[LCB_CLCONFIG_FILE] = lcb_clconfig_create_file(mon);
    mon->all_providers[LCB_CLCONFIG_CCCP] = lcb_clconfig_create_cccp(mon);
    mon->all_providers[LCB_CLCONFIG_HTTP] = lcb_clconfig_create_http(mon);
    mon->all_providers[LCB_CLCONFIG_USER] = lcb_clconfig_create_user(mon);

    for (ii = 0; ii < LCB_CLCONFIG_MAX; ii++) {
        mon->all_providers[ii]->parent = mon;
    }
    mon->as_stop = lcbio_timer_new(iot, mon, async_stop);
    mon->as_start = lcbio_timer_new(iot, mon, async_start);
    return mon;
}

void lcb_confmon_prepare(lcb_confmon *mon)
{
    int ii;

    memset(&mon->active_providers, 0, sizeof(mon->active_providers));
    lcb_clist_init(&mon->active_providers);

    for (ii = 0; ii < LCB_CLCONFIG_MAX; ii++) {
        clconfig_provider *cur = mon->all_providers[ii];
        if (cur) {
            if (cur->enabled) {
                lcb_clist_append(&mon->active_providers, &cur->ll);
                lcb_log(LOGARGS(mon, DEBUG), "Provider %s is ENABLED", provider_string(cur->type));
            } else if (cur->pause){
                cur->pause(cur);
                lcb_log(LOGARGS(mon, DEBUG), "Provider %s is DISABLED", provider_string(cur->type));
            }
        }
    }

    lcb_assert(LCB_CLIST_SIZE(&mon->active_providers));
    mon->cur_provider = first_active(mon);
}

void lcb_confmon_destroy(lcb_confmon *mon)
{
    unsigned int ii;

    if (mon->as_start) {
        lcbio_timer_destroy(mon->as_start);
    }

    if (mon->as_stop) {
        lcbio_timer_destroy(mon->as_stop);
    }

    mon->as_start = NULL;
    mon->as_stop = NULL;

    if (mon->config) {
        lcb_clconfig_decref(mon->config);
        mon->config = NULL;
    }

    for (ii = 0; ii < LCB_CLCONFIG_MAX; ii++) {
        clconfig_provider *provider = mon->all_providers[ii];
        if (provider == NULL) {
            continue;
        }

        provider->shutdown(provider);
        mon->all_providers[ii] = NULL;
    }

    lcbio_table_unref(mon->iot);
    lcb_settings_unref(mon->settings);

    free(mon);
}

static int do_set_next(lcb_confmon *mon, clconfig_info *info, int notify_miss)
{
    unsigned ii;

    if (mon->config) {
        VBUCKET_CHANGE_STATUS chstatus = VBUCKET_NO_CHANGES;
        VBUCKET_CONFIG_DIFF *diff = vbucket_compare(mon->config->vbc, info->vbc);

        if (!diff) {
            return 0;
        }

        chstatus = vbucket_what_changed(diff);
        vbucket_free_diff(diff);

        if (chstatus == 0 || lcb_clconfig_compare(mon->config, info) >= 0) {
            if (notify_miss) {
                invoke_listeners(mon, CLCONFIG_EVENT_GOT_ANY_CONFIG, info);
            }
            return 0;
        }
    }

    lcb_log(LOGARGS(mon, INFO), "Setting new configuration. Received via %s", provider_string(info->origin));

    if (mon->config) {
        /** DECREF the old one */
        lcb_clconfig_decref(mon->config);
    }

    for (ii = 0; ii < LCB_CLCONFIG_MAX; ii++) {
        clconfig_provider *cur = mon->all_providers[ii];
        if (cur && cur->enabled && cur->config_updated) {
            cur->config_updated(cur, info->vbc);
        }
    }

    lcb_clconfig_incref(info);
    mon->config = info;
    lcb_confmon_stop(mon);

    invoke_listeners(mon, CLCONFIG_EVENT_GOT_NEW_CONFIG, info);

    return 1;
}

void lcb_confmon_provider_failed(clconfig_provider *provider,
                                 lcb_error_t reason)
{
    lcb_confmon *mon = provider->parent;

    lcb_log(LOGARGS(mon, INFO), "Provider '%s' failed", provider_string(provider->type));

    if (provider != mon->cur_provider) {
        lcb_log(LOGARGS(mon, TRACE), "Ignoring failure. Current=%p (%s)", (void*)mon->cur_provider, provider_string(mon->cur_provider->type));
        return;
    }

    if (reason != LCB_SUCCESS) {
        mon->last_error = reason;
    }

    mon->cur_provider = next_active(mon, mon->cur_provider);

    if (!mon->cur_provider) {
        LOG(mon, TRACE, "Maximum provider reached. Resetting index");
        invoke_listeners(mon, CLCONFIG_EVENT_PROVIDERS_CYCLED, NULL);
        mon->cur_provider = first_active(mon);
        lcb_confmon_stop(mon);
    } else {
        mon->state |= CONFMON_S_ITERGRACE;
        lcbio_timer_rearm(mon->as_start, mon->settings->grace_next_provider);
    }
}

void lcb_confmon_provider_success(clconfig_provider *provider,
                                  clconfig_info *config)
{
    do_set_next(provider->parent, config, 1);
    lcb_confmon_stop(provider->parent);
}


static int do_next_provider(lcb_confmon *mon)
{
    lcb_list_t *ii;
    mon->state &= ~CONFMON_S_ITERGRACE;

    LCB_LIST_FOR(ii, (lcb_list_t *)&mon->active_providers) {
        clconfig_info *info;
        clconfig_provider *cached_provider;

        cached_provider = LCB_LIST_ITEM(ii, clconfig_provider, ll);
        info = cached_provider->get_cached(cached_provider);
        if (!info) {
            continue;
        }

        if (do_set_next(mon, info, 0)) {
            LOG(mon, DEBUG, "Using cached configuration");
            return 1;
        }
    }

    lcb_log(LOGARGS(mon, TRACE), "Current provider is %s", provider_string(mon->cur_provider->type));

    mon->cur_provider->refresh(mon->cur_provider);
    return 0;
}

static void async_start(void *arg)
{
    do_next_provider(arg);
}

lcb_error_t lcb_confmon_start(lcb_confmon *mon)
{
    lcb_uint32_t now_us, diff, tmonext;
    lcbio_async_cancel(mon->as_stop);
    if (IS_REFRESHING(mon)) {
        LOG(mon, DEBUG, "Refresh already in progress...");
        return LCB_SUCCESS;
    }

    LOG(mon, TRACE, "Start refresh requested");
    lcb_assert(mon->cur_provider);
    mon->state = CONFMON_S_ACTIVE|CONFMON_S_ITERGRACE;

    now_us = LCB_NS2US(gethrtime());
    diff = now_us - mon->last_stop_us;

    if (diff > mon->settings->grace_next_cycle) {
        tmonext = 0;
    } else {
        tmonext = mon->settings->grace_next_cycle - diff;
    }

    lcbio_timer_rearm(mon->as_start, tmonext);
    return LCB_SUCCESS;
}

static void async_stop(void *arg)
{
    lcb_confmon *mon = arg;
    lcb_list_t *ii;

    LCB_LIST_FOR(ii, (lcb_list_t *)&mon->active_providers) {
        clconfig_provider *provider = LCB_LIST_ITEM(ii, clconfig_provider, ll);
        if (!provider->pause) {
            continue;
        }
        provider->pause(provider);
    }

    mon->last_stop_us = LCB_NS2US(gethrtime());
    invoke_listeners(mon, CLCONFIG_EVENT_MONITOR_STOPPED, NULL);
}

lcb_error_t lcb_confmon_stop(lcb_confmon *mon)
{
    if (!IS_REFRESHING(mon)) {
        return LCB_SUCCESS;
    }
    lcbio_timer_disarm(mon->as_start);
    lcbio_async_signal(mon->as_stop);
    mon->state = CONFMON_S_INACTIVE;
    return LCB_SUCCESS;
}

void lcb_clconfig_decref(clconfig_info *info)
{
    lcb_assert(info->refcount);

    if (--info->refcount) {
        return;
    }

    if (info->vbc) {
        vbucket_config_destroy(info->vbc);
    }

    free(info);
}

int lcb_clconfig_compare(const clconfig_info *a, const clconfig_info *b)
{
    /** First check if both have revisions */
    int rev_a, rev_b;
    rev_a = vbucket_config_get_revision(a->vbc);
    rev_b = vbucket_config_get_revision(b->vbc);
    if (rev_a >= 0  && rev_b >= 0) {
        return rev_a - rev_b;
    }

    if (a->cmpclock == b->cmpclock) {
        return 0;

    } else if (a->cmpclock < b->cmpclock) {
        return -1;
    }

    return 1;
}

clconfig_info *
lcb_clconfig_create(VBUCKET_CONFIG_HANDLE config, clconfig_method_t origin)
{
    clconfig_info *info = calloc(1, sizeof(*info));
    if (!info) {
        return NULL;
    }
    info->refcount = 1;
    info->vbc = config;
    info->origin = origin;
    return info;
}

void lcb_confmon_add_listener(lcb_confmon *mon, clconfig_listener *listener)
{
    listener->parent = mon;
    lcb_list_append(&mon->listeners, &listener->ll);
}

void lcb_confmon_remove_listener(lcb_confmon *mon, clconfig_listener *listener)
{
    lcb_list_delete(&listener->ll);
    (void)mon;
}

static void invoke_listeners(lcb_confmon *mon,
                             clconfig_event_t event,
                             clconfig_info *info)
{
    lcb_list_t *ll, *ll_next;
    LCB_LIST_SAFE_FOR(ll, ll_next, &mon->listeners) {
        clconfig_listener *lsn = LCB_LIST_ITEM(ll, clconfig_listener, ll);
        lsn->callback(lsn, event, info);
    }
}

static void generic_shutdown(clconfig_provider *provider)
{
    free(provider);
}

clconfig_provider * lcb_clconfig_create_user(lcb_confmon *mon)
{
    clconfig_provider *provider = calloc(1, sizeof(*provider));
    provider->type = LCB_CLCONFIG_USER;
    provider->shutdown = generic_shutdown;

    (void)mon;
    return provider;
}

LCB_INTERNAL_API
int lcb_confmon_is_refreshing(lcb_confmon *mon)
{
    if(IS_REFRESHING(mon)) {
        LOG(mon, DEBUG, "Refresh already in progress...");
        return 1;
    }
    return 0;
}

LCB_INTERNAL_API
void
lcb_confmon_set_provider_active(lcb_confmon *mon,
    clconfig_method_t type, int enabled)
{
    clconfig_provider *provider = mon->all_providers[type];
    if (provider->enabled == enabled) {
        return;
    } else {
        provider->enabled = enabled;
    }
    lcb_confmon_prepare(mon);
}
