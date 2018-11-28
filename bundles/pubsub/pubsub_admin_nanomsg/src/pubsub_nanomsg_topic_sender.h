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
#ifndef CELIX_PUBSUB_ZMQ_TOPIC_SENDER_H
#define CELIX_PUBSUB_ZMQ_TOPIC_SENDER_H
#include <mutex>
#include <map>
#include "celix_bundle_context.h"
#include <log_helper.h>
#include <pubsub_serializer.h>
#include <pubsub/publisher.h>

namespace pubsub {
    namespace nanomsg {
        class pubsub_nanomsg_topic_sender;

        class bounded_service_entry {
        public:
            bounded_service_entry(
                    pubsub::nanomsg::pubsub_nanomsg_topic_sender *_parent,
                    long _bndId,
                    log_helper_t* _logHelper) : parent{_parent}, bndId{_bndId}, logHelper{_logHelper} {

            }

            int topicPublicationSend(unsigned int msgTypeId, const void *inMsg);

            pubsub::nanomsg::pubsub_nanomsg_topic_sender *parent{};
            pubsub_publisher_t service{};
            long bndId{};
            hash_map_t *msgTypes{};
            int getCount{1};
            log_helper_t *logHelper{};
        } ;


        class pubsub_nanomsg_topic_sender {
        public:
            pubsub_nanomsg_topic_sender(celix_bundle_context_t *_ctx, log_helper_t *_logHelper, const char *_scope,
                                        const char *_topic, long _serializerSvcId, pubsub_serializer_service_t *_ser,
                                        const char *_bindIp, unsigned int _basePort, unsigned int _maxPort);

            ~pubsub_nanomsg_topic_sender();

            pubsub_nanomsg_topic_sender(const pubsub_nanomsg_topic_sender &) = delete;

            const pubsub_nanomsg_topic_sender &operator=(const pubsub_nanomsg_topic_sender &) = delete;

            long getSerializerSvcId() const ;
            const std::string &getScope() const ;
            const std::string &getTopic() const ;
            const std::string &getUrl() const;

            void* getPublisherService(const celix_bundle_t *requestingBundle,
                                      const celix_properties_t *svcProperties __attribute__((unused)));
            void ungetPublisherService(const celix_bundle_t *requestingBundle,
                                       const celix_properties_t *svcProperties __attribute__((unused)));
            int topicPublicationSend(unsigned int msgTypeId, const void *inMsg);
            void delay_first_send_for_late_joiners() ;

            //private:
            celix_bundle_context_t *ctx;
            log_helper_t *logHelper;
            long serializerSvcId;
            pubsub_serializer_service_t *serializer;

            std::string scope{};
            std::string topic{};
            std::string scopeAndTopicFilter{};
            std::string url{};

            struct {
                std::mutex mutex;
                int socket;
            } nanomsg{};

            struct {
                long svcId;
                celix_service_factory_t factory;
            } publisher{};

            struct {
                std::mutex mutex{};
                std::map<long, bounded_service_entry> map{};
                //hash_map_t *map{};  //key = bndId, value = psa_nanomsg_bounded_service_entry_t
            } boundedServices{};
        };
    }
}

#endif //CELIX_PUBSUB_ZMQ_TOPIC_SENDER_H
