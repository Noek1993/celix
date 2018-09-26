/**
 *Licensed to the Apache Software Foundation (ASF) under one
 *or more contributor license agreements.  See the NOTICE file
 *distributed with this work for additional information
 *regarding copyright ownership.  The ASF licenses this file
 *to you under the Apache License, Version 2.0 (the
 *"License"); you may not use this file except in compliance
 *with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing,
 *software distributed under the License is distributed on an
 *"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 *specific language governing permissions and limitations
 *under the License.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <netdb.h>
#include <netinet/in.h>
#include <jansson.h>
#include <assert.h>
#include <sys/time.h>

#include "celix_bundle_context.h"
#include "celix_properties.h"
#include "constants.h"
#include "celix_threads.h"
#include "array_list.h"
#include "utils.h"
#include "celix_errno.h"
#include "filter.h"

#include "pubsub_listeners.h"
#include "pubsub_endpoint.h"
#include "pubsub_discovery_impl.h"

static celix_properties_t* pubsub_discovery_parseEndpoint(const char *value);
static char* pubsub_discovery_createJsonEndpoint(const celix_properties_t *props);
static void pubsub_discovery_addDiscoveredEndpoint(pubsub_discovery_t *disc, celix_properties_t *endpoint);
static void pubsub_discovery_removeDiscoveredEndpoint(pubsub_discovery_t *disc, const char *uuid);

/* Discovery activator functions */
celix_status_t pubsub_discovery_create(bundle_context_pt context, pubsub_discovery_t **ps_discovery) {
    celix_status_t status = CELIX_SUCCESS;

    *ps_discovery = calloc(1, sizeof(**ps_discovery));

    if (*ps_discovery == NULL) {
        return CELIX_ENOMEM;
    }

    (*ps_discovery)->context = context;
    (*ps_discovery)->discoveredEndpoints = hashMap_create(utils_stringHash, NULL, utils_stringEquals, NULL);
    (*ps_discovery)->announcedEndpoints = hashMap_create(utils_stringHash, NULL, utils_stringEquals, NULL);
    (*ps_discovery)->discoveredEndpointsListeners = hashMap_create(NULL, NULL, NULL, NULL);
    celixThreadMutex_create(&(*ps_discovery)->discoveredEndpointsListenersMutex, NULL);
    celixThreadMutex_create(&(*ps_discovery)->announcedEndpointsMutex, NULL);
    celixThreadMutex_create(&(*ps_discovery)->discoveredEndpointsMutex, NULL);
    pthread_mutex_init(&(*ps_discovery)->waitMutex, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&(*ps_discovery)->waitCond, &attr);
    celixThreadMutex_create(&(*ps_discovery)->runningMutex, NULL);
    (*ps_discovery)->running = true;


    (*ps_discovery)->verbose = celix_bundleContext_getPropertyAsBool(context, PUBSUB_ETCD_DISCOVERY_VERBOSE_KEY, PUBSUB_ETCD_DISCOVERY_DEFAULT_VERBOSE);

    const char* etcdIp = celix_bundleContext_getProperty(context, PUBSUB_DISCOVERY_SERVER_IP_KEY, PUBSUB_DISCOVERY_SERVER_IP_DEFAULT);
    long etcdPort = celix_bundleContext_getPropertyAsLong(context, PUBSUB_DISCOVERY_SERVER_PORT_KEY, PUBSUB_DISCOVERY_SERVER_PORT_DEFAULT);
    long ttl = celix_bundleContext_getPropertyAsLong(context, PUBSUB_DISCOVERY_ETCD_TTL_KEY, PUBSUB_DISCOVERY_ETCD_TTL_DEFAULT);

    etcd_init(etcdIp, (int)etcdPort, ETCDLIB_NO_CURL_INITIALIZATION);
    (*ps_discovery)->ttlForEntries = (int)ttl;
    (*ps_discovery)->sleepInsecBetweenTTLRefresh = (int)(((float)ttl)/2.0);
    (*ps_discovery)->pubsubPath = celix_bundleContext_getProperty(context, PUBSUB_DISCOVERY_SERVER_PATH_KEY, PUBSUB_DISCOVERY_SERVER_PATH_DEFAULT);
    (*ps_discovery)->fwUUID = celix_bundleContext_getProperty(context, OSGI_FRAMEWORK_FRAMEWORK_UUID, NULL);

    return status;
}

celix_status_t pubsub_discovery_destroy(pubsub_discovery_t *ps_discovery) {
    celix_status_t status = CELIX_SUCCESS;

    celixThreadMutex_lock(&ps_discovery->discoveredEndpointsMutex);
    hashMap_destroy(ps_discovery->discoveredEndpoints, false, false);
    celixThreadMutex_unlock(&ps_discovery->discoveredEndpointsMutex);
    celixThreadMutex_destroy(&ps_discovery->discoveredEndpointsMutex);

    celixThreadMutex_lock(&ps_discovery->discoveredEndpointsListenersMutex);
    hashMap_destroy(ps_discovery->discoveredEndpointsListeners, false, false);
    celixThreadMutex_unlock(&ps_discovery->discoveredEndpointsListenersMutex);
    celixThreadMutex_destroy(&ps_discovery->discoveredEndpointsListenersMutex);

    celixThreadMutex_lock(&ps_discovery->announcedEndpointsMutex);
    hashMap_destroy(ps_discovery->announcedEndpoints, false, false);
    celixThreadMutex_unlock(&ps_discovery->announcedEndpointsMutex);
    celixThreadMutex_destroy(&ps_discovery->announcedEndpointsMutex);

    pthread_mutex_destroy(&ps_discovery->waitMutex);
    pthread_cond_destroy(&ps_discovery->waitCond);

    celixThreadMutex_destroy(&ps_discovery->runningMutex);

    free(ps_discovery);

    return status;
}


static void psd_etcdReadCallback(const char *key __attribute__((unused)), const char *value, void* arg) {
    pubsub_discovery_t *disc = arg;
    celix_properties_t *props = pubsub_discovery_parseEndpoint(value);
    if (props != NULL) {
        pubsub_discovery_addDiscoveredEndpoint(disc, props);
    }
}

static void psd_watchSetupConnection(pubsub_discovery_t *disc, bool *connectedPtr, long long *mIndex) {
    bool connected = *connectedPtr;
    if (!connected) {
        if (disc->verbose) {
            printf("[PSD] Reading etcd directory at %s\n", disc->pubsubPath);
        }
        int rc = etcd_get_directory(disc->pubsubPath, psd_etcdReadCallback, disc, mIndex);
        if (rc == ETCDLIB_RC_OK) {
            *connectedPtr = true;
        } else {
            *connectedPtr = false;
        }
    }
}

static void psd_watchForChange(pubsub_discovery_t *disc, bool *connectedPtr, long long *mIndex) {
    bool connected = *connectedPtr;
    if (connected) {
        long long watchIndex = *mIndex + 1;

        char *action = NULL;
        char *value = NULL;
        char *readKey = NULL;
        //TODO add interruptable etcd_wait -> which returns a handle to interrupt and a can be used for a wait call
        int rc = etcd_watch(disc->pubsubPath, watchIndex, &action, NULL, &value, &readKey, mIndex);
        if (rc == ETCDLIB_RC_TIMEOUT) {
            //nop
        } else if (rc == ETCDLIB_RC_ERROR || action == NULL) {
            printf("[PSD] Error communicating with etcd. rc is %i, action value is %s\n", rc, action);
            *connectedPtr = false;
        } else {
            if (strncmp(ETCDLIB_ACTION_CREATE, action, strlen(ETCDLIB_ACTION_CREATE)) == 0 ||
                       strncmp(ETCDLIB_ACTION_SET, action, strlen(ETCDLIB_ACTION_SET)) == 0 ||
                       strncmp(ETCDLIB_ACTION_UPDATE, action, strlen(ETCDLIB_ACTION_UPDATE)) == 0) {
                celix_properties_t *props = pubsub_discovery_parseEndpoint(value);
                if (props != NULL) {
                    pubsub_discovery_addDiscoveredEndpoint(disc, props);
                }
            } else if (strncmp(ETCDLIB_ACTION_DELETE, action, strlen(ETCDLIB_ACTION_DELETE)) == 0 ||
                       strncmp(ETCDLIB_ACTION_EXPIRE, action, strlen(ETCDLIB_ACTION_EXPIRE)) == 0) {
                char *uuid = strrchr(readKey, '/');
                if (uuid != NULL) {
                    uuid = uuid + 1;
                    pubsub_discovery_removeDiscoveredEndpoint(disc, uuid);
                }
            } else {
                //ETCDLIB_ACTION_GET -> nop
            }

            free(action);
            free(value);
            free(readKey);
        }
    } else {
        if (disc->verbose) {
            printf("[PSD] Skipping etcd watch -> not connected\n");
        }
    }
}

static void psd_cleanupIfDisconnected(pubsub_discovery_t *disc, bool *connectedPtr) {
    bool connected = *connectedPtr;
    if (!connected) {

        celixThreadMutex_lock(&disc->discoveredEndpointsMutex);
        int size = hashMap_size(disc->discoveredEndpoints);
        if (disc->verbose) {
            printf("[PSD] Removing all discovered entries (%i) -> not connected\n", size);
        }

        hash_map_iterator_t iter = hashMapIterator_construct(disc->discoveredEndpoints);
        while (hashMapIterator_hasNext(&iter)) {
            celix_properties_t *endpoint = hashMapIterator_nextValue(&iter);

            celixThreadMutex_lock(&disc->discoveredEndpointsListenersMutex);
            hash_map_iterator_t iter2 = hashMapIterator_construct(disc->discoveredEndpointsListeners);
            while (hashMapIterator_hasNext(&iter2)) {
                pubsub_discovered_endpoint_listener_t *listener = hashMapIterator_nextValue(&iter2);
                listener->removeDiscoveredEndpoint(listener->handle, endpoint);
            }
            celixThreadMutex_unlock(&disc->discoveredEndpointsListenersMutex);

            celix_properties_destroy(endpoint);
        }
        hashMap_clear(disc->discoveredEndpoints, false, false);
        celixThreadMutex_unlock(&disc->discoveredEndpointsMutex);
    }
}

void* psd_watch(void *data) {
    pubsub_discovery_t *disc = data;

    long long mIndex = 0L;
    bool connected = false;

    celixThreadMutex_lock(&disc->runningMutex);
    bool running = disc->running;
    celixThreadMutex_unlock(&disc->runningMutex);

    while (running) {
        psd_watchSetupConnection(disc, &connected, &mIndex);
        psd_watchForChange(disc, &connected, &mIndex);
        psd_cleanupIfDisconnected(disc, &connected);

        if (!connected) {
            usleep(5000000); //if not connected wait a few seconds
        }

        celixThreadMutex_lock(&disc->runningMutex);
        running = disc->running;
        celixThreadMutex_unlock(&disc->runningMutex);
    }

    return NULL;
}

void* psd_refresh(void *data) {
    pubsub_discovery_t *disc = data;

    celixThreadMutex_lock(&disc->runningMutex);
    bool running = disc->running;
    celixThreadMutex_unlock(&disc->runningMutex);

    while (running) {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);

        celixThreadMutex_lock(&disc->announcedEndpointsMutex);
        hash_map_iterator_t iter = hashMapIterator_construct(disc->announcedEndpoints);
        while (hashMapIterator_hasNext(&iter)) {
            pubsub_announce_entry_t *entry = hashMapIterator_nextValue(&iter);
            if (entry->isSet) {
                //only refresh ttl -> no index update -> no watch trigger
                int rc = etcd_refresh(entry->key, disc->ttlForEntries);
                if (rc != ETCDLIB_RC_OK) {
                    fprintf(stderr, "[PSD] Warning: error refreshing etcd key %s\n", entry->key);
                    entry->isSet = false;
                }
            } else {
                char *str = pubsub_discovery_createJsonEndpoint(entry->properties);
                int rc = etcd_set(entry->key, str, disc->ttlForEntries, false);
                if (rc == ETCDLIB_RC_OK) {
                    entry->isSet = true;
                } else {
                    fprintf(stderr, "[PSD] Warning: error setting endpoint in etcd for key %s\n", entry->key);
                }
            }
        }
        celixThreadMutex_unlock(&disc->announcedEndpointsMutex);

        struct timespec waitTill = start;
        waitTill.tv_sec += disc->sleepInsecBetweenTTLRefresh;
        pthread_mutex_lock(&disc->waitMutex);
        pthread_cond_timedwait(&disc->waitCond, &disc->waitMutex, &waitTill); //TODO add timedwait abs for celixThread (including MONOTONIC ..)
        pthread_mutex_unlock(&disc->waitMutex);

        celixThreadMutex_lock(&disc->runningMutex);
        running = disc->running;
        celixThreadMutex_unlock(&disc->runningMutex);
    }
    return NULL;
}

celix_status_t pubsub_discovery_start(pubsub_discovery_t *ps_discovery) {
    celix_status_t status = CELIX_SUCCESS;

    celixThread_create(&ps_discovery->watchThread, NULL, psd_watch, ps_discovery);
    celixThread_setName(&ps_discovery->watchThread, "PubSub ETCD Watch");
    celixThread_create(&ps_discovery->refreshTTLThread, NULL, psd_refresh, ps_discovery);
    celixThread_setName(&ps_discovery->refreshTTLThread, "PubSub ETCD Refresh TTL");

    return status;
}

celix_status_t pubsub_discovery_stop(pubsub_discovery_t *disc) {
    celix_status_t status = CELIX_SUCCESS;

    celixThreadMutex_lock(&disc->runningMutex);
    disc->running = false;
    celixThreadMutex_unlock(&disc->runningMutex);

    celixThreadMutex_lock(&disc->waitMutex);
    celixThreadCondition_broadcast(&disc->waitCond);
    celixThreadMutex_unlock(&disc->waitMutex);

    celixThread_join(disc->watchThread, NULL);
    celixThread_join(disc->refreshTTLThread, NULL);

    celixThreadMutex_lock(&disc->discoveredEndpointsMutex);
    hash_map_iterator_t iter = hashMapIterator_construct(disc->discoveredEndpoints);
    while (hashMapIterator_hasNext(&iter)) {
        celix_properties_t *props = hashMapIterator_nextValue(&iter);

        celixThreadMutex_lock(&disc->discoveredEndpointsListenersMutex);
        hash_map_iterator_t iter2 = hashMapIterator_construct(disc->discoveredEndpointsListeners);
        while (hashMapIterator_hasNext(&iter2)) {
            pubsub_discovered_endpoint_listener_t *listener = hashMapIterator_nextValue(&iter2);
            listener->removeDiscoveredEndpoint(listener->handle, props);
        }
        celixThreadMutex_unlock(&disc->discoveredEndpointsListenersMutex);

        celix_properties_destroy(props);
    }
    hashMap_clear(disc->discoveredEndpoints, false, false);
    celixThreadMutex_unlock(&disc->discoveredEndpointsMutex);

    celixThreadMutex_lock(&disc->announcedEndpointsMutex);
    iter = hashMapIterator_construct(disc->announcedEndpoints);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_announce_entry_t *entry = hashMapIterator_nextValue(&iter);
        if (entry->isSet) {
            etcd_del(entry->key);
        }
        celix_properties_destroy(entry->properties);
        free(entry);
    }
    hashMap_clear(disc->announcedEndpoints, false, false);
    celixThreadMutex_unlock(&disc->announcedEndpointsMutex);

    return status;
}

void pubsub_discovery_discoveredEndpointsListenerAdded(void *handle, void *svc, const celix_properties_t *props, const celix_bundle_t *bnd) {
    pubsub_discovery_t *disc = handle;
    pubsub_discovered_endpoint_listener_t *listener = svc;

    long svcId = celix_properties_getAsLong(props, OSGI_FRAMEWORK_SERVICE_ID, -1L);
    celixThreadMutex_lock(&disc->discoveredEndpointsListenersMutex);
    hashMap_put(disc->discoveredEndpointsListeners, (void*)svcId, listener);
    celixThreadMutex_unlock(&disc->discoveredEndpointsListenersMutex);

    celixThreadMutex_lock(&disc->discoveredEndpointsMutex);
    hash_map_iterator_t iter = hashMapIterator_construct(disc->discoveredEndpoints);
    while (hashMapIterator_hasNext(&iter)) {
        celix_properties_t *props = hashMapIterator_nextValue(&iter);
        listener->addDiscoveredEndpoint(listener->handle, props);
    }
    celixThreadMutex_unlock(&disc->discoveredEndpointsMutex);
}

void pubsub_discovery_discoveredEndpointsListenerRemoved(void *handle, void *svc, const celix_properties_t *props, const celix_bundle_t *bnd) {
    pubsub_discovery_t *disc = handle;
    pubsub_discovered_endpoint_listener_t *listener = svc;

    long svcId = celix_properties_getAsLong(props, OSGI_FRAMEWORK_SERVICE_ID, -1L);
    celixThreadMutex_lock(&disc->discoveredEndpointsListenersMutex);
    hashMap_put(disc->discoveredEndpointsListeners, (void*)svcId, listener);
    celixThreadMutex_unlock(&disc->discoveredEndpointsListenersMutex);
}

celix_status_t pubsub_discovery_announceEndpoint(void *handle, const celix_properties_t *endpoint) {
    pubsub_discovery_t *disc = handle;
    celix_status_t status = CELIX_SUCCESS;

    bool valid = pubsubEndpoint_isValid(endpoint, true, true);
    const char *config = celix_properties_get(endpoint, PUBSUB_ENDPOINT_ADMIN_TYPE, NULL);
    const char *scope = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_SCOPE, NULL);
    const char *topic = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_NAME, NULL);
    const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, NULL);

    if (valid) {
        pubsub_announce_entry_t *entry = calloc(1, sizeof(*entry));
        entry->isSet = false;
        entry->properties = celix_properties_copy(endpoint);
        asprintf(&entry->key, "/pubsub/%s/%s/%s/%s", config, scope, topic, uuid);

        const char *hashKey = celix_properties_get(entry->properties, PUBSUB_ENDPOINT_UUID, NULL);
        celixThreadMutex_lock(&disc->announcedEndpointsMutex);
        hashMap_put(disc->announcedEndpoints, (void*)hashKey, entry);
        celixThreadMutex_unlock(&disc->announcedEndpointsMutex);

        celixThreadMutex_lock(&disc->waitMutex);
        celixThreadCondition_broadcast(&disc->waitCond);
        celixThreadMutex_unlock(&disc->waitMutex);
    } else {
        printf("[PSD] Error cannot announce endpoint. missing some mandatory properties\n");
    }

    return status;
}
celix_status_t pubsub_discovery_removeEndpoint(void *handle, const celix_properties_t *endpoint) {
    pubsub_discovery_t *disc = handle;
    celix_status_t status = CELIX_SUCCESS;

    const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, NULL);
    pubsub_announce_entry_t *entry = NULL;

    if (uuid != NULL) {
        celixThreadMutex_lock(&disc->announcedEndpointsMutex);
        entry = hashMap_remove(disc->announcedEndpoints, uuid);
        celixThreadMutex_unlock(&disc->announcedEndpointsMutex);
    } else {
        printf("[PSD] Error cannot remove announced endpoint. missing endpoint uuid property\n");
    }

    if (entry != NULL) {
        if (entry->isSet) {
            etcd_del(entry->key);
        }
        free(entry->key);
        celix_properties_destroy(entry->properties);
        free(entry);
    }

    return status;
}


static void pubsub_discovery_addDiscoveredEndpoint(pubsub_discovery_t *disc, celix_properties_t *endpoint) {
    const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, NULL);
    const char *fwUUID = celix_properties_get(endpoint, PUBSUB_ENDPOINT_FRAMEWORK_UUID, NULL);

    //note endpoint should already be check to be valid pubsubEndpoint_isValid
    assert(uuid != NULL);
    assert(fwUUID != NULL);

    if (fwUUID != NULL && strncmp(disc->fwUUID, fwUUID, strlen(disc->fwUUID)) == 0) {
//        if (disc->verbose) {
//            printf("[PSD] Ignoring endpoint %s from own framework\n", uuid);
//        }
        return;
    }

    if (disc->verbose) {
        const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, "!Error!");
        const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, "!Error!");
        const char *admin = celix_properties_get(endpoint, PUBSUB_ENDPOINT_ADMIN_TYPE, "!Error!");
        const char *ser = celix_properties_get(endpoint, PUBSUB_SERIALIZER_TYPE_KEY, "!Error!");
        printf("[PSD] Adding discovered endpoint %s. type is %s, admin is %s, serializer is %s.\n",
               uuid, type, admin, ser);
    }

    celixThreadMutex_lock(&disc->discoveredEndpointsMutex);
    bool exists = hashMap_containsKey(disc->discoveredEndpoints, (void*)uuid);
    hashMap_put(disc->discoveredEndpoints, (void*)uuid, endpoint);
    celixThreadMutex_unlock(&disc->discoveredEndpointsMutex);

    if (!exists) {
        celixThreadMutex_lock(&disc->discoveredEndpointsListenersMutex);
        hash_map_iterator_t iter = hashMapIterator_construct(disc->discoveredEndpointsListeners);
        while (hashMapIterator_hasNext(&iter)) {
            pubsub_discovered_endpoint_listener_t *listener = hashMapIterator_nextValue(&iter);
            listener->addDiscoveredEndpoint(listener->handle, endpoint);
        }
        celixThreadMutex_unlock(&disc->discoveredEndpointsListenersMutex);
    } else {
        //assuming this is the same endpoint -> ignore
    }
}

static void pubsub_discovery_removeDiscoveredEndpoint(pubsub_discovery_t *disc, const char *uuid) {
    celixThreadMutex_lock(&disc->discoveredEndpointsMutex);
    bool exists = hashMap_containsKey(disc->discoveredEndpoints, (void*)uuid);
    celix_properties_t *endpoint = hashMap_remove(disc->discoveredEndpoints, (void*)uuid);
    celixThreadMutex_unlock(&disc->discoveredEndpointsMutex);

    if (endpoint == NULL) {
        //NOTE assuming this was a endpoint from this framework -> ignore
        return;
    }

    if (disc->verbose) {
        const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, "!Error!");
        const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, "!Error!");
        const char *admin = celix_properties_get(endpoint, PUBSUB_ENDPOINT_ADMIN_TYPE, "!Error!");
        const char *ser = celix_properties_get(endpoint, PUBSUB_SERIALIZER_TYPE_KEY, "!Error!");
        printf("[PSD] Removing discovered endpoint %s. type is %s, admin is %s, serializer is %s.\n",
               uuid, type, admin, ser);
    }

    if (exists && endpoint != NULL) {
        celixThreadMutex_lock(&disc->discoveredEndpointsListenersMutex);
        hash_map_iterator_t iter = hashMapIterator_construct(disc->discoveredEndpointsListeners);
        while (hashMapIterator_hasNext(&iter)) {
            pubsub_discovered_endpoint_listener_t *listener = hashMapIterator_nextValue(&iter);
            listener->removeDiscoveredEndpoint(listener->handle, endpoint);
        }
        celixThreadMutex_unlock(&disc->discoveredEndpointsListenersMutex);
    } else {
        fprintf(stderr, "[PSD] Warning unexpected remove from non existing endpoint (uuid is %s)\n", uuid);
    }
}

celix_properties_t* pubsub_discovery_parseEndpoint(const char* etcdValue) {
    properties_t *props = properties_create();

    // etcdValue contains the json formatted string
    json_error_t error;
    json_t* jsonRoot = json_loads(etcdValue, JSON_DECODE_ANY, &error);

    if (json_is_object(jsonRoot)) {

        void *iter = json_object_iter(jsonRoot);

        const char *key;
        json_t *value;

        while (iter) {
            key = json_object_iter_key(iter);
            value = json_object_iter_value(iter);
            properties_set(props, key, json_string_value(value));
            iter = json_object_iter_next(jsonRoot, iter);
        }
    }

    if (jsonRoot != NULL) {
        json_decref(jsonRoot);
    }

    bool valid = pubsubEndpoint_isValid(props, true, true);
    if (!valid) {
        fprintf(stderr, "[PSD] Warning retrieved endpoint is not valid\n");
        celix_properties_destroy(props);
        props = NULL;
    }

    return props;
}

static char* pubsub_discovery_createJsonEndpoint(const celix_properties_t *props) {
    //note props is already check for validity (pubsubEndpoint_isValid)

    json_t *jsEndpoint = json_object();
    const char* propKey = NULL;
    PROPERTIES_FOR_EACH((celix_properties_t*)props, propKey) {
        const char* val = celix_properties_get(props, propKey, NULL);
        json_object_set_new(jsEndpoint, propKey, json_string(val));
    }
    char* str = json_dumps(jsEndpoint, JSON_COMPACT);
    json_decref(jsEndpoint);
    return str;
}

celix_status_t pubsub_discovery_executeCommand(void *handle, char * commandLine __attribute__((unused)), FILE *os, FILE *errorStream __attribute__((unused))) {
    //pubsub_discovery_t *psd = handle;
    //TODO
    fprintf(os, "TODO\n");
    return CELIX_SUCCESS;
}