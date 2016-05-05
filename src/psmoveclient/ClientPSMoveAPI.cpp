//-- includes -----
#include "ClientPSMoveAPI.h"
#include "ClientRequestManager.h"
#include "ClientNetworkManager.h"
#include "ClientControllerView.h"
#include "PSMoveProtocol.pb.h"
#include <iostream>
#include <map>
#include <deque>

//-- typedefs -----
typedef std::map<int, ClientControllerView *> t_controller_view_map;
typedef std::map<int, ClientControllerView *>::iterator t_controller_view_map_iterator;
typedef std::pair<int, ClientControllerView *> t_id_controller_view_pair;
typedef std::deque<ClientPSMoveAPI::Message> t_message_queue;
typedef std::vector<ResponsePtr> t_response_reference_cache;
typedef std::vector<ResponsePtr> t_event_reference_cache;

//-- internal implementation -----
class ClientPSMoveAPIImpl : 
    public IDataFrameListener,
    public INotificationListener,
    public IClientNetworkEventListener
{
public:
    ClientPSMoveAPIImpl(
        const std::string &host, 
        const std::string &port)
        : m_request_manager(ClientPSMoveAPIImpl::enqueue_response_message, this)
        , m_network_manager(
            host, port, 
            this, // IDataFrameListener
            this, // INotificationListener
            &m_request_manager, // IResponseListener
            this) // IClientNetworkEventListener
        , m_controller_view_map()
    {
    }

    virtual ~ClientPSMoveAPIImpl()
    {
        // Without this we get a warning for deletion:
        // "Delete called on 'class ClientPSMoveAPIImpl' that has virtual functions but non-virtual destructor"
    }

    // -- ClientPSMoveAPI System -----
    bool startup(e_log_severity_level log_level)
    {
        bool success = true;

        log_init(log_level);

        // Attempt to connect to the server
        if (success)
        {
            if (!m_network_manager.startup())
            {
                CLIENT_LOG_ERROR("ClientPSMoveAPI") << "Failed to initialize the client network manager" << std::endl;
                success = false;
            }
        }

        if (success)
        {
            CLIENT_LOG_INFO("ClientPSMoveAPI") << "Successfully initialized ClientPSMoveAPI" << std::endl;
        }

        return success;
    }

    void update()
    {
        // Drop an unread messages from the previous call to update
        m_message_queue.clear();

        // Drop all of the message parameters
        // NOTE: std::vector::clear() calls the destructor on each element in the vector
        // This will decrement the last ref count to the parameter data, causing them to get cleaned up.
        m_response_reference_cache.clear();
        m_event_reference_cache.clear();

        // Process incoming/outgoing networking requests
        m_network_manager.update();
    }

    bool poll_next_message(ClientPSMoveAPI::Message *message, size_t message_size)
    {
        bool bHasMessage = false;

        if (m_message_queue.size() > 0)
        {
            const ClientPSMoveAPI::Message &first = m_message_queue.front();

            assert(sizeof(ClientPSMoveAPI::Message) == message_size);
            assert(message != nullptr);
            memcpy(message, &first, sizeof(ClientPSMoveAPI::Message));

            m_message_queue.pop_front();

            // NOTE: We intentionally keep the message parameters around in the 
            // m_response_reference_cache and m_event_reference_cache since the
            // messages contain raw void pointers to the parameters, which
            // become invalid after the next call to update.

            bHasMessage = true;
        }

        return bHasMessage;
    }

    void shutdown()
    {
        // Close all active network connections
        m_network_manager.shutdown();

        // Drop an unread messages from the previous call to update
        m_message_queue.clear();

        // Drop all of the message parameters
        // NOTE: std::vector::clear() calls the destructor on each element in the vector
        // This will decrement the last ref count to the parameter data, causing them to get cleaned up.
        m_response_reference_cache.clear();
        m_event_reference_cache.clear();
    }

    // -- ClientPSMoveAPI Requests -----
    ClientControllerView * allocate_controller_view(int ControllerID)
    {
        ClientControllerView * view;

        // Use the same view if one already exists for the given controller id
        t_controller_view_map_iterator view_entry= m_controller_view_map.find(ControllerID);
        if (view_entry != m_controller_view_map.end())
        {
            view= view_entry->second;
        }
        else
        {
            // Create a new initialized controller view
            view= new ClientControllerView(ControllerID);

            // Add it to the map of controller
            m_controller_view_map.insert(t_id_controller_view_pair(ControllerID, view));
        }

        // Keep track of how many clients are listening to this view
        view->IncListenerCount();        
        
        return view;
    }

    void free_controller_view(ClientControllerView * view)
    {
        t_controller_view_map_iterator view_entry= m_controller_view_map.find(view->GetControllerID());
        assert(view_entry != m_controller_view_map.end());

        // Decrease the number of listeners to this view
        view->DecListenerCount();

        // If no one is listening to this controller anymore, free it from the map
        if (view->GetListenerCount() <= 0)
        {
            // Free the controller view allocated in allocate_controller_view
            delete view_entry->second;
            view_entry->second= nullptr;

            // Remove the entry from the map
            m_controller_view_map.erase(view_entry);
        }
    }

    ClientPSMoveAPI::t_request_id start_controller_data_stream(ClientControllerView * view, unsigned int flags)
    {
        CLIENT_LOG_INFO("start_controller_data_stream") << "requesting controller stream start for PSMoveID: " << view->GetControllerID() << std::endl;

        // Tell the psmove service that we are acquiring this controller
        RequestPtr request(new PSMoveProtocol::Request());
        request->set_type(PSMoveProtocol::Request_RequestType_START_CONTROLLER_DATA_STREAM);
        request->mutable_request_start_psmove_data_stream()->set_controller_id(view->GetControllerID());

        if ((flags & ClientPSMoveAPI::includeRawSensorData) > 0)
        {
            request->mutable_request_start_psmove_data_stream()->set_include_raw_sensor_data(true);
        }

        m_request_manager.send_request(request);

        return request->request_id();
    }

    ClientPSMoveAPI::t_request_id stop_controller_data_stream(ClientControllerView * view)
    {
        CLIENT_LOG_INFO("stop_controller_data_stream") << "requesting controller stream stop for PSMoveID: " << view->GetControllerID() << std::endl;

        // Tell the psmove service that we are releasing this controller
        RequestPtr request(new PSMoveProtocol::Request());
        request->set_type(PSMoveProtocol::Request_RequestType_STOP_CONTROLLER_DATA_STREAM);
        request->mutable_request_stop_psmove_data_stream()->set_controller_id(view->GetControllerID());

        m_request_manager.send_request(request);

        return request->request_id();
    }

    ClientPSMoveAPI::t_request_id set_controller_rumble(ClientControllerView * view, float rumble_amount)
    {
        CLIENT_LOG_INFO("set_controller_rumble") << "request set rumble to " << rumble_amount << " for PSMoveID: " << view->GetControllerID() << std::endl;

        assert(m_controller_view_map.find(view->GetControllerID()) != m_controller_view_map.end());

        // Tell the psmove service to set the rumble controller
        // Internally rumble values are in the range [0, 255]
        RequestPtr request(new PSMoveProtocol::Request());
        request->set_type(PSMoveProtocol::Request_RequestType_SET_RUMBLE);
        request->mutable_request_rumble()->set_controller_id(view->GetControllerID());
        request->mutable_request_rumble()->set_rumble(static_cast<int>(rumble_amount * 255.f));

        m_request_manager.send_request(request);

        return request->request_id();
    }

    ClientPSMoveAPI::t_request_id set_led_color(
        ClientControllerView *view, 
        unsigned char r, unsigned char g, unsigned b)
    {
        CLIENT_LOG_INFO("set_controller_rumble") << "request set color to " << r << "," << g << "," << b << 
            " for PSMoveID: " << view->GetControllerID() << std::endl;

        assert(m_controller_view_map.find(view->GetControllerID()) != m_controller_view_map.end());

        // Tell the psmove service to set the rumble controller
        // Internally rumble values are in the range [0, 255]
        RequestPtr request(new PSMoveProtocol::Request());
        request->set_type(PSMoveProtocol::Request_RequestType_SET_LED_COLOR);
        request->mutable_set_led_color_request()->set_controller_id(view->GetControllerID());
        request->mutable_set_led_color_request()->set_r(static_cast<int>(r));
        request->mutable_set_led_color_request()->set_g(static_cast<int>(g));
        request->mutable_set_led_color_request()->set_b(static_cast<int>(b));

        m_request_manager.send_request(request);

        return request->request_id();
    }

    ClientPSMoveAPI::t_request_id reset_pose(ClientControllerView * view)
    {
        CLIENT_LOG_INFO("set_controller_rumble") << "requesting pose reset for PSMoveID: " << view->GetControllerID() << std::endl;

        // Tell the psmove service to set the current orientation of the given controller as the identity pose
        RequestPtr request(new PSMoveProtocol::Request());
        request->set_type(PSMoveProtocol::Request_RequestType_RESET_POSE);
        request->mutable_reset_pose()->set_controller_id(view->GetControllerID());
        
        m_request_manager.send_request(request);

        return request->request_id();
    }

    ClientPSMoveAPI::t_request_id send_opaque_request(
        ClientPSMoveAPI::t_request_handle request_handle)
    {
        RequestPtr &request= *reinterpret_cast<RequestPtr *>(request_handle);

        m_request_manager.send_request(request);

        return request->request_id();
    }

    // IDataFrameListener
    virtual void handle_data_frame(ControllerDataFramePtr data_frame) override
    {
        CLIENT_LOG_TRACE("handle_data_frame") << "received data frame for ControllerID: " << data_frame->controller_id() << std::endl;

        t_controller_view_map_iterator view_entry= m_controller_view_map.find(data_frame->controller_id());

        if (view_entry != m_controller_view_map.end())
        {
            ClientControllerView * view= view_entry->second;

            view->ApplyControllerDataFrame(data_frame.get());
        }
    }

    // INotificationListener
    virtual void handle_notification(ResponsePtr notification) override
    {
        assert(notification->request_id() == -1);

        ClientPSMoveAPI::eClientPSMoveAPIEvent specificEventType= ClientPSMoveAPI::opaqueServiceEvent;

        // See if we can translate this to an event type a client without protocol access can see
        switch(notification->type())
        {
        case PSMoveProtocol::Response_ResponseType_CONTROLLER_LIST_UPDATED:
            specificEventType= ClientPSMoveAPI::controllerListUpdated;
            break;
        case PSMoveProtocol::Response_ResponseType_TRACKER_LIST_UPDATED:
            specificEventType = ClientPSMoveAPI::trackerListUpdated;
            break;
        case PSMoveProtocol::Response_ResponseType_HMD_LIST_UPDATED:
            specificEventType = ClientPSMoveAPI::hmdListUpdated;
            break;
        }

        enqueue_event_message(specificEventType, notification);
    }

    // IClientNetworkEventListener
    virtual void handle_server_connection_opened() override
    {
        CLIENT_LOG_INFO("handle_server_connection_opened") << "Connected to service" << std::endl;

        enqueue_event_message(ClientPSMoveAPI::connectedToService, ResponsePtr());
    }

    virtual void handle_server_connection_open_failed(const boost::system::error_code& ec) override
    {
        CLIENT_LOG_ERROR("handle_server_connection_open_failed") << "Failed to connect to service: " << ec.message() << std::endl;

        enqueue_event_message(ClientPSMoveAPI::failedToConnectToService, ResponsePtr());
    }

    virtual void handle_server_connection_closed() override
    {
        CLIENT_LOG_INFO("handle_server_connection_closed") << "Disconnected from service" << std::endl;

        enqueue_event_message(ClientPSMoveAPI::disconnectedFromService, ResponsePtr());
    }

    virtual void handle_server_connection_close_failed(const boost::system::error_code& ec) override
    {
        CLIENT_LOG_ERROR("handle_server_connection_close_failed") << "Error disconnecting from service: " << ec.message() << std::endl;
    }

    virtual void handle_server_connection_socket_error(const boost::system::error_code& ec) override
    {
        CLIENT_LOG_ERROR("handle_server_connection_close_failed") << "Socket error: " << ec.message() << std::endl;
    }

    // Request Manager Callback
    static void enqueue_response_message(
        ClientPSMoveAPI::eClientPSMoveResultCode result_code,
        const ClientPSMoveAPI::t_request_id request_id,
        ResponsePtr response,
        void *userdata)
    {
        ClientPSMoveAPIImpl *this_ptr= reinterpret_cast<ClientPSMoveAPIImpl *>(userdata);
        ClientPSMoveAPI::Message message;

        memset(&message, 0, sizeof(ClientPSMoveAPI::Message));
        message.payload_type = ClientPSMoveAPI::_messagePayloadType_Response;
        message.response_data.request_id = request_id;
        message.response_data.result_code = result_code;
        //NOTE: This pointer is only safe until the next update call to update is made
        message.response_data.response_handle = (bool)response ? static_cast<const void *>(response.get()) : nullptr;

        // Add the message to the message queue
        this_ptr->m_message_queue.push_back(message);

        // Maintain a reference to the response until the next update
        if (response)
        {
            this_ptr->m_response_reference_cache.push_back(response);
        }
    }

    void enqueue_event_message(
        ClientPSMoveAPI::eClientPSMoveAPIEvent event_type,
        ResponsePtr event)
    {
        ClientPSMoveAPI::Message message;

        memset(&message, 0, sizeof(ClientPSMoveAPI::Message));
        message.payload_type = ClientPSMoveAPI::_messagePayloadType_Event;
        message.event_data.event_type= event_type;
        //NOTE: This pointer is only safe until the next update call to update is made
        message.event_data.event_data_handle = (bool)event ? static_cast<const void *>(event.get()) : nullptr;

        // Add the message to the message queue
        m_message_queue.push_back(message);

        // Maintain a reference to the event until the next update
        if (event)
        {
            m_event_reference_cache.push_back(event);
        }
    }

private:
    ClientRequestManager m_request_manager;
    ClientNetworkManager m_network_manager;
    
    t_controller_view_map m_controller_view_map;

    // Queue of message received from the most recent call to update()
    // This queue will be emptied automatically at the next call to update().
    t_message_queue m_message_queue;

    // These vectors are used solely to keep the ref counted pointers to the 
    // response and event parameter data valid until the next update call.
    // The message queue contains raw void pointers to the response and event data.
    t_response_reference_cache m_response_reference_cache;
    t_event_reference_cache m_event_reference_cache;
};

//-- ClientPSMoveAPI -----
class ClientPSMoveAPIImpl *ClientPSMoveAPI::m_implementation_ptr = nullptr;

bool ClientPSMoveAPI::startup(
    const std::string &host, 
    const std::string &port,
    e_log_severity_level log_level)
{
    bool success= true;

    if (ClientPSMoveAPI::m_implementation_ptr == nullptr)
    {
        ClientPSMoveAPI::m_implementation_ptr = new ClientPSMoveAPIImpl(host, port);
        success= ClientPSMoveAPI::m_implementation_ptr->startup(log_level);
    }

    return success;
}

bool ClientPSMoveAPI::has_started()
{
    return ClientPSMoveAPI::m_implementation_ptr != nullptr;
}

void ClientPSMoveAPI::update()
{
    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        ClientPSMoveAPI::m_implementation_ptr->update();
    }
}

bool ClientPSMoveAPI::poll_next_message(ClientPSMoveAPI::Message *message, size_t message_size)
{
    bool bResult = false;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        bResult= ClientPSMoveAPI::m_implementation_ptr->poll_next_message(message, message_size);
    }

    return bResult;
}

void ClientPSMoveAPI::shutdown()
{
    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        ClientPSMoveAPI::m_implementation_ptr->shutdown();
        
        delete ClientPSMoveAPI::m_implementation_ptr;

        ClientPSMoveAPI::m_implementation_ptr = nullptr;
    }
}

ClientControllerView * ClientPSMoveAPI::allocate_controller_view(int ControllerID)
{
    ClientControllerView * view;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        view= ClientPSMoveAPI::m_implementation_ptr->allocate_controller_view(ControllerID);
    }

    return view;
}

void ClientPSMoveAPI::free_controller_view(ClientControllerView * view)
{
    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        ClientPSMoveAPI::m_implementation_ptr->free_controller_view(view);
    }
}

ClientPSMoveAPI::t_request_id 
ClientPSMoveAPI::start_controller_data_stream(
    ClientControllerView * view, 
    unsigned int flags)
{
    ClientPSMoveAPI::t_request_id request_id= ClientPSMoveAPI::INVALID_REQUEST_ID;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        request_id= ClientPSMoveAPI::m_implementation_ptr->start_controller_data_stream(view, flags);
    }

    return request_id;
}

ClientPSMoveAPI::t_request_id 
ClientPSMoveAPI::stop_controller_data_stream(
    ClientControllerView * view)
{
    ClientPSMoveAPI::t_request_id request_id= ClientPSMoveAPI::INVALID_REQUEST_ID;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        request_id= ClientPSMoveAPI::m_implementation_ptr->stop_controller_data_stream(view);
    }

    return request_id;
}

ClientPSMoveAPI::t_request_id 
ClientPSMoveAPI::set_controller_rumble(
    ClientControllerView * view, 
    float rumble_amount)
{
    ClientPSMoveAPI::t_request_id request_id= ClientPSMoveAPI::INVALID_REQUEST_ID;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        request_id= ClientPSMoveAPI::m_implementation_ptr->set_controller_rumble(view, rumble_amount);
    }

    return request_id;
}

ClientPSMoveAPI::t_request_id 
ClientPSMoveAPI::set_led_color(
    ClientControllerView *view, 
    unsigned char r, unsigned char g, unsigned b)
{
    ClientPSMoveAPI::t_request_id request_id= ClientPSMoveAPI::INVALID_REQUEST_ID;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        request_id= ClientPSMoveAPI::m_implementation_ptr->set_led_color(view, r, g, b);
    }

    return request_id;
}

ClientPSMoveAPI::t_request_id 
ClientPSMoveAPI::reset_pose(
    ClientControllerView * view)
{
    ClientPSMoveAPI::t_request_id request_id= ClientPSMoveAPI::INVALID_REQUEST_ID;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        request_id= ClientPSMoveAPI::m_implementation_ptr->reset_pose(view);
    }

    return request_id;
}

ClientPSMoveAPI::t_request_id 
ClientPSMoveAPI::send_opaque_request(
    ClientPSMoveAPI::t_request_handle request_handle)
{
    ClientPSMoveAPI::t_request_id request_id= ClientPSMoveAPI::INVALID_REQUEST_ID;

    if (ClientPSMoveAPI::m_implementation_ptr != nullptr)
    {
        request_id= ClientPSMoveAPI::m_implementation_ptr->send_opaque_request(request_handle);
    }

    return request_id;
}
