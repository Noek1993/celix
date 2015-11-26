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
/*
 * remote_service_admin_http_impl.h
 *
 *  \date       Sep 30, 2011
 *  \author    	<a href="mailto:dev@celix.apache.org">Apache Celix Project Team</a>
 *  \copyright	Apache License, Version 2.0
 */

#ifndef REMOTE_SERVICE_ADMIN_HTTP_IMPL_H_
#define REMOTE_SERVICE_ADMIN_HTTP_IMPL_H_


#include "bundle_context.h"
#include "endpoint_description.h"

//typedef struct remote_service_admin *remote_service_admin_pt;

celix_status_t remoteServiceAdmin_create(bundle_context_pt context, remote_service_admin_pt *admin);
celix_status_t remoteServiceAdmin_destroy(remote_service_admin_pt *admin);

celix_status_t remoteServiceAdmin_stop(remote_service_admin_pt admin);

celix_status_t remoteServiceAdmin_exportService(remote_service_admin_pt admin, char *serviceId, properties_pt properties, array_list_pt *registrations);
celix_status_t remoteServiceAdmin_removeExportedService(remote_service_admin_pt admin, export_registration_pt registration);
celix_status_t remoteServiceAdmin_getExportedServices(remote_service_admin_pt admin, array_list_pt *services);
celix_status_t remoteServiceAdmin_getImportedEndpoints(remote_service_admin_pt admin, array_list_pt *services);
celix_status_t remoteServiceAdmin_importService(remote_service_admin_pt admin, endpoint_description_pt endpoint, import_registration_pt *registration);
celix_status_t remoteServiceAdmin_removeImportedService(remote_service_admin_pt admin, import_registration_pt registration);


celix_status_t exportReference_getExportedEndpoint(export_reference_pt reference, endpoint_description_pt *endpoint);
celix_status_t exportReference_getExportedService(export_reference_pt reference, service_reference_pt *service);

celix_status_t importReference_getImportedEndpoint(import_reference_pt reference);
celix_status_t importReference_getImportedService(import_reference_pt reference);

celix_status_t remoteServiceAdmin_destroyEndpointDescription(endpoint_description_pt *description);

#endif /* REMOTE_SERVICE_ADMIN_HTTP_IMPL_H_ */
