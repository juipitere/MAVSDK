#include "global_include.h"
#include "device_impl.h"
#include "dronelink_impl.h"
#include <unistd.h>

namespace dronelink {

using namespace std::placeholders; // for `_1`


DeviceImpl::DeviceImpl(DroneLinkImpl *parent) :
    _mavlink_handler_table(),
    _timeout_handler_map_mutex(),
    _timeout_handler_map(),
    _target_system_id(0),
    _target_component_id(0),
    _target_uuid(0),
    _target_supports_mission_int(false),
    _parent(parent),
    _command_result(MAV_RESULT_FAILED),
    _command_state(CommandState::NONE),
    _command_result_callback(nullptr),
    _device_thread(nullptr),
    _should_exit(false),
    _timeout_s(DEFAULT_TIMEOUT_S),
    _last_heartbeat_received_time(),
    _heartbeat_timeout_s(DEFAULT_HEARTBEAT_TIMEOUT_S),
    _heartbeat_timed_out(false)
{
    register_mavlink_message_handler(
        MAVLINK_MSG_ID_HEARTBEAT,
        std::bind(&DeviceImpl::process_heartbeat, this, _1), this);

    register_mavlink_message_handler(
        MAVLINK_MSG_ID_COMMAND_ACK,
        std::bind(&DeviceImpl::process_command_ack, this, _1), this);

    register_mavlink_message_handler(
        MAVLINK_MSG_ID_AUTOPILOT_VERSION,
        std::bind(&DeviceImpl::process_autopilot_version, this, _1), this);
}

DeviceImpl::~DeviceImpl()
{
    _should_exit = true;
    unregister_all_mavlink_message_handlers(this);

    if (_device_thread != nullptr) {
        _device_thread->join();
        delete _device_thread;
        _device_thread = nullptr;
    }
}

void DeviceImpl::register_mavlink_message_handler(uint8_t msg_id,
                                                  mavlink_message_handler_t callback,
                                                  const void *cookie)
{
    MavlinkHandlerTableEntry entry = {msg_id, callback, cookie};
    _mavlink_handler_table.push_back(entry);
}

void DeviceImpl::unregister_all_mavlink_message_handlers(const void *cookie)
{
    for (auto it = _mavlink_handler_table.begin();
         it != _mavlink_handler_table.end();
         /* no ++it */) {

        if (it->cookie == cookie) {
            it = _mavlink_handler_table.erase(it);
        } else {
            ++it;
        }
    }
}

void DeviceImpl::register_timeout_handler(timeout_handler_t callback,
                                          const void *cookie)
{
    std::lock_guard<std::mutex> lock(_timeout_handler_map_mutex);

    dl_time_t future_time = steady_time_in_future(_timeout_s);

    TimeoutHandlerMapEntry entry = {future_time, callback};
    _timeout_handler_map.insert({cookie, entry});
}

void DeviceImpl::update_timeout_handler(const void *cookie)
{
    std::lock_guard<std::mutex> lock(_timeout_handler_map_mutex);

    auto it = _timeout_handler_map.find(cookie);
    if (it != _timeout_handler_map.end()) {
        dl_time_t future_time = steady_time_in_future(_timeout_s);
        it->second.time = future_time;
    }
}

void DeviceImpl::unregister_timeout_handler(const void *cookie)
{
    std::lock_guard<std::mutex> lock(_timeout_handler_map_mutex);

    auto it = _timeout_handler_map.find(cookie);
    if (it != _timeout_handler_map.end()) {
        _timeout_handler_map.erase(cookie);
    }
}

void DeviceImpl::process_mavlink_message(const mavlink_message_t &message)
{
    for (auto it = _mavlink_handler_table.begin(); it != _mavlink_handler_table.end(); ++it) {
        if (it->msg_id == message.msgid) {
            it->callback(message);
        }
    }
}

void DeviceImpl::process_heartbeat(const mavlink_message_t &message)
{
    mavlink_heartbeat_t heartbeat;
    mavlink_msg_heartbeat_decode(&message, &heartbeat);

    if (_target_system_id == 0) {
        _target_system_id = message.sysid;
        _target_component_id = message.compid;
    }
    if (_target_uuid == 0) {
        request_autopilot_version();
    }

    check_device_thread();

    _last_heartbeat_received_time = steady_time();
    _heartbeat_timed_out = false;
}

void DeviceImpl::process_command_ack(const mavlink_message_t &message)
{
    mavlink_command_ack_t command_ack;
    mavlink_msg_command_ack_decode(&message, &command_ack);

    // Ignore it if we're not waiting for an ack result.
    if (_command_state == CommandState::WAITING) {
        _command_result = (MAV_RESULT)command_ack.result;
        // Update state after result to avoid a race over _command_result
        _command_state = CommandState::RECEIVED;

        if (_command_result == MAV_RESULT_ACCEPTED) {
            if (_command_result_callback != nullptr) {
                report_result(_command_result_callback, CommandResult::SUCCESS);
            }
        } else {
            if (_command_result_callback != nullptr) {
                report_result(_command_result_callback, CommandResult::SUCCESS);
            }
        }
        _command_result_callback = nullptr;
    }
}

void DeviceImpl::process_autopilot_version(const mavlink_message_t &message)
{
    mavlink_autopilot_version_t autopilot_version;
    mavlink_msg_autopilot_version_decode(&message, &autopilot_version);

    if (_target_uuid == 0) {
        _target_uuid = autopilot_version.uid;
        _target_supports_mission_int =
            autopilot_version.capabilities & MAV_PROTOCOL_CAPABILITY_MISSION_INT;

        _parent->notify_on_discover(_target_uuid);

    } else if (_target_uuid != autopilot_version.uid) {
        // TODO: raise a flag to invalidate device
        Debug() << "Error: UUID changed";
    }
}

void DeviceImpl::check_device_thread()
{
    if (_device_thread == nullptr) {
        _device_thread = new std::thread(device_thread, this);
    }
}

void DeviceImpl::device_thread(DeviceImpl *self)
{
    const unsigned heartbeat_interval_us = 1000000;
    const unsigned timeout_interval_us = 10000;
    const unsigned heartbeat_multiplier = heartbeat_interval_us / timeout_interval_us;
    unsigned counter = 0;

    while (!self->_should_exit) {
        if (counter++ % heartbeat_multiplier == 0) {
            send_heartbeat(self);
        }
        check_timeouts(self);
        check_heartbeat_timeout(self);
        usleep(timeout_interval_us);
    }
}

void DeviceImpl::send_heartbeat(DeviceImpl *self)
{
    mavlink_message_t message;
    mavlink_msg_heartbeat_pack(_own_system_id, _own_component_id, &message,
                               MAV_TYPE_GCS, 0, 0, 0, 0);
    self->send_message(message);
}

void DeviceImpl::check_timeouts(DeviceImpl *self)
{
    timeout_handler_t callback = nullptr;

    {
        std::lock_guard<std::mutex> lock(self->_timeout_handler_map_mutex);

        for (auto it = self->_timeout_handler_map.begin();
             it != self->_timeout_handler_map.end(); /* no ++it */) {

            // If time is passed, call timeout callback.
            if (it->second.time < steady_time()) {

                callback = it->second.callback;
                //Self-destruct before calling to avoid locking issues.
                self->_timeout_handler_map.erase(it++);
                break;

            } else {
                ++it;
            }
        }
    }

    // Now that the lock is out of scope and therefore unlocked, we're safe
    // to call the callback if set which might in turn register new timeout callbacks.
    if (callback != nullptr) {
        callback();
    }
}

void DeviceImpl::check_heartbeat_timeout(DeviceImpl *self)
{
    if (elapsed_s(self->_last_heartbeat_received_time) > self->_heartbeat_timeout_s) {
        if (!self->_heartbeat_timed_out) {
            self->_parent->notify_on_timeout(self->_target_uuid);
            self->_heartbeat_timed_out = true;
        }
    }
}

bool DeviceImpl::send_message(const mavlink_message_t &message)
{
    return _parent->send_message(message);
}

void DeviceImpl::request_autopilot_version()
{
    send_command(MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES,
                 CommandParams {1.0f, NAN, NAN, NAN, NAN, NAN, NAN});
}

uint64_t DeviceImpl::get_target_uuid() const
{
    return _target_uuid;
}

uint8_t DeviceImpl::get_target_system_id() const
{
    return _target_system_id;
}

uint8_t DeviceImpl::get_target_component_id() const
{
    return _target_component_id;
}

DeviceImpl::CommandResult DeviceImpl::send_command(uint16_t command,
                                                   const DeviceImpl::CommandParams &params)
{
    if (_target_system_id == 0 && _target_component_id == 0) {
        return CommandResult::NO_DEVICE;
    }

    // We don't need no ack, just send it.
    //if (_command_state == CommandState::WAITING) {
    //    return Result::DEVICE_BUSY;
    //}

    mavlink_message_t message;

    mavlink_msg_command_long_pack(_own_system_id, _own_component_id,
                                  &message,
                                  _target_system_id, _target_component_id,
                                  command,
                                  0,
                                  params.v[0], params.v[1], params.v[2], params.v[3],
                                  params.v[4], params.v[5], params.v[6]);

    if (!_parent->send_message(message)) {
        return CommandResult::CONNECTION_ERROR;
    }

    return CommandResult::SUCCESS;
}

DeviceImpl::CommandResult DeviceImpl::send_command_with_ack(
    uint16_t command, const DeviceImpl::CommandParams &params)
{
    if (_command_state == CommandState::WAITING) {
        return CommandResult::BUSY;
    }

    _command_result_callback = nullptr;

    _command_state = CommandState::WAITING;

    CommandResult ret = send_command(command, params);
    if (ret != CommandResult::SUCCESS) {
        return ret;
    }

    const unsigned timeout_us = unsigned(_timeout_s * 1e6);
    const unsigned wait_time_us = 1000;
    const unsigned iterations = timeout_us / wait_time_us;

    // Wait until we have received a result.
    for (unsigned i = 0; i < iterations; ++i) {
        if (_command_state == CommandState::RECEIVED) {
            break;
        }
        usleep(wait_time_us);
    }

    if (_command_state != CommandState::RECEIVED) {
        _command_state = CommandState::NONE;
        return CommandResult::TIMEOUT;
    }

    // Reset
    _command_state = CommandState::NONE;

    if (_command_result != MAV_RESULT_ACCEPTED) {
        return CommandResult::COMMAND_DENIED;
    }

    return CommandResult::SUCCESS;
}

void DeviceImpl::send_command_with_ack_async(uint16_t command,
                                             const DeviceImpl::CommandParams &params,
                                             command_result_callback_t callback)
{
    if (_command_state == CommandState::WAITING) {
        report_result(callback, CommandResult::BUSY);
    }

    CommandResult ret = send_command(command, params);
    if (ret != CommandResult::SUCCESS) {
        report_result(callback, ret);
        // Reset
        _command_state = CommandState::NONE;
        return;
    }

    if (callback == nullptr) {
        Debug() << "Callback is null";
    }

    _command_state = CommandState::WAITING;
    _command_result_callback = callback;
}

DeviceImpl::CommandResult DeviceImpl::set_msg_rate(uint16_t message_id, double rate_hz)
{
    float interval_us = 1e6f / rate_hz;
    return send_command_with_ack(
               MAV_CMD_SET_MESSAGE_INTERVAL,
               CommandParams {float(message_id), interval_us, NAN, NAN, NAN, NAN, NAN});
}

void DeviceImpl::report_result(const command_result_callback_t &callback, CommandResult result)
{
    if (!callback) {
        return;
    }

    callback(result);
}

} // namespace dronelink
