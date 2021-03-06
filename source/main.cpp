/*
 * Copyright (c) 2015 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h> /* For SIGIGN and SIGINT */
#include "mbed-client/m2minterfacefactory.h"
#include "mbed-client/m2mdevice.h"
#include "mbed-client/m2minterfaceobserver.h"
#include "mbed-client/m2minterface.h"
#include "mbed-client/m2mobjectinstance.h"
#include "mbed-client/m2mresource.h"
#include "mbed-client/m2mblockmessage.h"
#include "security.h"
#include "mbed-trace/mbed_trace.h"

//Select binding mode: UDP or TCP
M2MInterface::BindingMode SOCKET_MODE = M2MInterface::UDP;

// This is address to mbed Device Connector
const String &MBED_SERVER_ADDRESS = "coap://api.connector.mbed.com:5684";

const String &MBED_USER_NAME_DOMAIN = MBED_DOMAIN;
const String &ENDPOINT_NAME = MBED_ENDPOINT_NAME;
const String &MANUFACTURER = "manufacturer";
const String &TYPE = "type";
const String &MODEL_NUMBER = "2015";
const String &SERIAL_NUMBER = "12345";

const uint8_t value[] = "MyValue";
const uint8_t STATIC_VALUE[] = "Static value";

static void ctrl_c_handle_function(void);
typedef void (*signalhandler_t)(int); /* Function pointer type for ctrl-c */
pthread_t unregister_thread;
pthread_t observation_thread;
pthread_t update_register_thread;
volatile bool loop;


class MbedClient: public M2MInterfaceObserver {
public:
    MbedClient(){
        _security = NULL;
        _interface = NULL;
        _device = NULL;
        _object = NULL;
        _error = false;
        _registered = false;
        _unregistered = false;
        _registration_updated = false;
        _value = 0;
        _block_message = NULL;
        _block_message_len = 0;
    }

    ~MbedClient() {
        if(_security) {
            delete _security;
        }
        if( _register_security){
            delete _register_security;
        }
        if(_device) {
            M2MDevice::delete_instance();
        }
        if(_object) {
            delete _object;
        }
        if(_interface) {
            delete _interface;
        }
        if(_block_message) {
            delete _block_message;
        }
    }

    bool create_interface() {

       time_t t;
       srand((unsigned) time(&t));
       // Port range from 1024 up to uint16
       uint16_t port = rand() % 64511 + 1024;

       _interface = M2MInterfaceFactory::create_interface(*this,
                                                  ENDPOINT_NAME,
                                                  "test",
                                                  100,
                                                  port,
                                                  MBED_USER_NAME_DOMAIN,
                                                  SOCKET_MODE,
                                                  M2MInterface::LwIP_IPv4,
                                                  "");
        printf("Endpoint Name : %s\n", ENDPOINT_NAME.c_str());
        return (_interface == NULL) ? false : true;
    }

    bool register_successful() {
        while(!_registered && !_error) {
            sleep(1);
        }
        return _registered;
    }

    bool unregister_successful() {
        while(!_unregistered && !_error) {
            sleep(1);
        }
        return _unregistered;
    }

    bool registration_update_successful() {
        while(!_registration_updated && !_error) {
        }
        return _registration_updated;
    }

    bool create_register_object() {
        bool success = false;
        _register_security = M2MInterfaceFactory::create_security(M2MSecurity::M2MServer);
        if(_register_security) {
            _register_security->set_resource_value(M2MSecurity::M2MServerUri, MBED_SERVER_ADDRESS);
            _register_security->set_resource_value(M2MSecurity::SecurityMode, M2MSecurity::Certificate);
            _register_security->set_resource_value(M2MSecurity::ServerPublicKey,SERVER_CERT,sizeof(SERVER_CERT));
            _register_security->set_resource_value(M2MSecurity::PublicKey,CERT,sizeof(CERT));
            _register_security->set_resource_value(M2MSecurity::Secretkey,KEY,sizeof(KEY));
            success = true;
        }
        return success;
    }

    bool create_device_object() {
        bool success = false;
        _device = M2MInterfaceFactory::create_device();
        if(_device) {
            if(_device->create_resource(M2MDevice::Manufacturer,MANUFACTURER)     &&
               _device->create_resource(M2MDevice::DeviceType,TYPE)        &&
               _device->create_resource(M2MDevice::ModelNumber,MODEL_NUMBER)      &&
               _device->create_resource(M2MDevice::SerialNumber,SERIAL_NUMBER)) {
                success = true;
            }
        }
        return success;
    }

    void execute_function(void *argument) {
        if(argument) {
            M2MResource::M2MExecuteParameter* param = (M2MResource::M2MExecuteParameter*)argument;
            String object_name = param->get_argument_object_name();
            uint16_t object_instance_id = param->get_argument_object_instance_id();
            String resource_name = param->get_argument_resource_name();
            int payload_length = param->get_argument_value_length();
            uint8_t* payload = param->get_argument_value();
            printf("Resource: %s/%d/%s executed\n", object_name.c_str(), object_instance_id, resource_name.c_str());
            printf("Payload: %.*s\n", payload_length, payload);
        }
    }

    bool create_generic_object() {
        bool success = false;
        _object = M2MInterfaceFactory::create_object("Test");
        if(_object) {
            M2MObjectInstance* inst = _object->create_object_instance();
            if(inst) {
                inst->set_operation(M2MBase::GET_PUT_POST_DELETE_ALLOWED);
                inst->set_register_uri(true);
                M2MResource* res = inst->create_dynamic_resource("D",
                                                                 "ResourceTest",
                                                                 M2MResourceInstance::INTEGER,
                                                                 true);
                char buffer[20];
                int size = sprintf(buffer,"%d",_value);
                res->set_operation(M2MBase::GET_PUT_POST_DELETE_ALLOWED);
                res->set_value((const uint8_t*)buffer,
                             (const uint32_t)size);
                res->set_execute_function(execute_callback(this,&MbedClient::execute_function));
                res->set_incoming_block_message_callback(incoming_block_message_callback(this, &MbedClient::block_message_received));
                res->set_outgoing_block_message_callback(outgoing_block_message_callback(this, &MbedClient::block_message_requested));
                _value++;
                inst->create_static_resource("S",
                                             "ResourceTest",
                                             M2MResourceInstance::STRING,
                                             STATIC_VALUE,
                                             sizeof(STATIC_VALUE)-1);
            }
        }
        return success;
    }

    void update_resource() {
        if(_object) {
            M2MObjectInstance* inst = _object->object_instance();
            if(inst) {
                M2MResource* res = inst->resource("D");
                printf("Resource Value /Test/0/D : %d\n", _value);
                char buffer[20];
                int size = sprintf(buffer,"%d",_value);
                res->set_value((const uint8_t*)buffer,
                               (const uint32_t)size);
                _value++;
            }
        }
    }

    void test_register(){
        M2MObjectList object_list;
        object_list.push_back(_device);
        object_list.push_back(_object);
        if(_interface) {
            _interface->register_object(_register_security,object_list);
        } else {
            printf("Interface doesn't exist, exiting!!\n");
            exit(1);
        }
    }

    void test_update_register() {
        uint32_t updated_lifetime = 100;
        if(_interface) {
            _interface->update_registration(_register_security,updated_lifetime);
        }
    }

    void test_unregister() {
        if(_interface) {
            _interface->unregister_object(NULL);
        }
    }

    void bootstrap_done(M2MSecurity */*server_object*/){
    }

    void object_registered(M2MSecurity */*security_object*/, const M2MServer &/*server_object*/){
        _registered = true;
        printf("\nRegistered\n");
    }

    void object_unregistered(M2MSecurity */*server_object*/){
        _unregistered = true;
        printf("\nUnregistered\n");
    }

    void registration_updated(M2MSecurity */*security_object*/, const M2MServer & /*server_object*/){
        _registration_updated = true;
        printf("\nregistration updated\n");

    }

    void error(M2MInterface::Error error){
        _error = true;
        String error_code;
        switch(error) {
            case M2MInterface::ErrorNone:
                error_code += "M2MInterface::ErrorNone";
                break;
            case M2MInterface::AlreadyExists:
                error_code += "M2MInterface::AlreadyExists";
                break;
            case M2MInterface::BootstrapFailed:
                error_code += "M2MInterface::BootstrapFailed";
                break;
            case M2MInterface::InvalidParameters:
                error_code += "M2MInterface::InvalidParameters";
                break;
            case M2MInterface::NotRegistered:
                error_code += "M2MInterface::NotRegistered";
                break;
            case M2MInterface::Timeout:
                error_code += "M2MInterface::Timeout";
                break;
            case M2MInterface::NetworkError:
                error_code += "M2MInterface::NetworkError";
                break;
            case M2MInterface::ResponseParseFailed:
                error_code += "M2MInterface::ResponseParseFailed";
                break;
            case M2MInterface::UnknownError:
                error_code += "M2MInterface::UnknownError";
                break;
            case M2MInterface::MemoryFail:
                error_code += "M2MInterface::MemoryFail";
                break;
            case M2MInterface::NotAllowed:
                error_code += "M2MInterface::NotAllowed";
                break;
            case M2MInterface::SecureConnectionFailed:
                error_code += "M2MInterface::SecureConnectionFailed";
                break;
            case M2MInterface::DnsResolvingFailed:
                error_code += "M2MInterface::DnsResolvingFailed";
                break;
            }
        printf("\nError occured  : %s\n", error_code.c_str());
    }

    void value_updated(M2MBase *base, M2MBase::BaseType type) {
        M2MResource* resource = NULL;
        String object_name = "";
        String resource_name = "";
        uint16_t object_instance_id = 0;
        if(base) {
            switch(base->base_type()) {
                case M2MBase::Resource: {
                    resource = (M2MResource*)base;
                    object_name = resource->object_name();
                    object_instance_id = resource->object_instance_id();
                    resource_name = resource->name();
                    printf("Resource: %s/%d/%s value updated\r\n",
                           resource->object_name().c_str(), resource->object_instance_id(), resource->name().c_str());
                }
                break;
            default:
                break;
            }
        }
    }

    void block_message_received(M2MBlockMessage *argument) {
        if (argument) {
            if (M2MBlockMessage::ErrorNone == argument->error_code()) {
                if (argument->is_last_block()) {
                    printf("Last block received\r\n");
                }
                printf("Block number: %d\r\n", argument->block_number());
                printf("Total message size: %d\r\n", argument->total_message_size());
                // First block arrived
                if (argument->block_number() == 0) {
                    free(_block_message);
                    _block_message = NULL;
                    _block_message_len = argument->total_message_size();
                    _block_message = (uint8_t*)malloc(_block_message_len);
                    if (_block_message) {
                        memcpy(_block_message, argument->block_data(), argument->block_data_len());
                    }
                } else {
                    if (_block_message) {
                        // Combine blocks
                        memcpy(_block_message + (argument->block_data_len() * argument->block_number()),
                               argument->block_data(),
                               argument->block_data_len());
                    }
                }
            } else {
                printf("Error when receiving block message! %d\r\n", argument->error_code());
            }
        }
    }

    void block_message_requested(const String& resource, uint8_t *&data, uint32_t &len) {
        printf("GET request received for resource: %s\r\n", resource.c_str());
        data = (uint8_t*)malloc(_block_message_len);
        len = _block_message_len;
        memcpy(data, _block_message, _block_message_len);
    }

private:

    M2MInterface        *_interface;
    M2MSecurity         *_security;
    M2MSecurity         *_register_security;
    M2MDevice           *_device;
    M2MObject           *_object;
    bool                _error;
    bool                _registered;
    bool                _unregistered;
    bool                _registration_updated;
    int                 _value;
    uint8_t             *_block_message;
    uint32_t            _block_message_len;
};

void* wait_for_unregister(void* arg) {
    MbedClient *client;
    client = (MbedClient*) arg;
    if(client->unregister_successful()) {
        printf("Unregistered done\n");
        loop = false;
        pthread_detach(update_register_thread);
        pthread_detach(observation_thread);
        pthread_detach(unregister_thread);

        pthread_cancel(update_register_thread);
        pthread_cancel(observation_thread);
        pthread_cancel(unregister_thread);
    }
    return NULL;
}

void* send_observation(void* arg) {
    MbedClient *client;
    client = (MbedClient*) arg;
    static uint8_t counter = 0;
    while(1) {
        sleep(1);
        if(counter >= 10 &&
           client->register_successful()) {
            printf("Incrementing Resource Value\n");
            client->update_resource();
            counter = 0;
        }
        else
            counter++;
    }
    return NULL;
}

void* update_register(void* arg) {
    MbedClient *client;
    client = (MbedClient*) arg;
    static uint8_t counter = 0;
    while(1) {
        sleep(50);
        if(client->register_successful()) {
            printf("Sending update register\n");
            client->test_update_register();
        }
    }
    return NULL;
}

static MbedClient *mbedclient = NULL;

static void ctrl_c_handle_function(void)
{
    if(mbedclient && mbedclient->register_successful()) {
        printf("\nUnregistering endpoint and EXITING Program\n");
        mbedclient->test_unregister();
    } else {
       printf("\nEXITING Program\n");
       exit(1);
    }
}

int main() {
    loop = true;
    signal(SIGINT, (signalhandler_t)ctrl_c_handle_function);
    MbedClient mbed_client;

    mbedclient = &mbed_client;

    mbed_trace_init();

    bool result = mbed_client.create_interface();
    if(true == result) {
        printf("\nInterface created\n");
    }
    result = mbed_client.create_register_object();
    if(true == result) {
        printf("register object created");
    }

    result = mbed_client.create_device_object();
    if(true == result){
        printf("\nDevice object created !!\n");
    }

    result = mbed_client.create_generic_object();

    if(true == result) {
        printf("\nGeneric object created\n");
    }

    printf("Registering endpoint\n");
    mbed_client.test_register();

    pthread_create(&observation_thread, NULL, &send_observation, (void*) &mbed_client);
    pthread_create(&unregister_thread, NULL, &wait_for_unregister, (void*) &mbed_client);
    pthread_create(&update_register_thread, NULL, &update_register, (void*) &mbed_client);

    while(loop);
    return 0;
}

