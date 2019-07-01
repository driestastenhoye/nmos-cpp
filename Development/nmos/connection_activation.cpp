#include "nmos/connection_activation.h"

#include "detail/for_each_reversed.h"
#include "nmos/activation_mode.h"
#include "nmos/connection_api.h" // for nmos::set_connection_resource_active, etc.
#include "nmos/model.h"
#include "nmos/slog.h"
#include "nmos/thread_utils.h"

namespace nmos
{
    void connection_activation_thread(nmos::node_model& model, connection_resource_auto_resolver resolve_auto, connection_sender_transportfile_setter set_transportfile, slog::base_gate& gate)
    {
        auto lock = model.write_lock(); // in order to update the resources

        auto most_recent_update = nmos::tai_min();
        auto earliest_scheduled_activation = (nmos::tai_clock::time_point::max)();

        for (;;)
        {
            // wait for the thread to be interrupted because there may be new scheduled activations, or immediate activations to process
            // or because the server is being shut down
            // or because it's time for the next scheduled activation
            model.wait_until(lock, earliest_scheduled_activation, [&] { return model.shutdown || most_recent_update < nmos::most_recent_update(model.connection_resources); });
            if (model.shutdown) break;

            auto& by_updated = model.connection_resources.get<nmos::tags::updated>();

            // go through all connection resources
            // process any immediate activations
            // process any scheduled activations whose requested_time has passed
            // identify the next scheduled activation

            const auto now = nmos::tai_clock::now();

            earliest_scheduled_activation = (nmos::tai_clock::time_point::max)();

            bool notify = false;

            // since modify reorders the resource in this index, use for_each_reversed
            detail::for_each_reversed(by_updated.begin(), by_updated.end(), [&](const nmos::resource& resource)
            {
                if (!resource.has_data()) return;

                const std::pair<nmos::id, nmos::type> id_type{ resource.id, resource.type };

                auto& staged = nmos::fields::endpoint_staged(resource.data);
                auto& staged_activation = nmos::fields::activation(staged);
                auto& staged_mode_or_null = nmos::fields::mode(staged_activation);

                if (staged_mode_or_null.is_null()) return;

                const nmos::activation_mode staged_mode{ staged_mode_or_null.as_string() };

                if (nmos::activation_modes::activate_scheduled_absolute == staged_mode ||
                    nmos::activation_modes::activate_scheduled_relative == staged_mode)
                {
                    auto& staged_activation_time = nmos::fields::activation_time(staged_activation);
                    const auto scheduled_activation = nmos::time_point_from_tai(nmos::parse_version(staged_activation_time.as_string()));

                    if (scheduled_activation < now)
                    {
                        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Processing scheduled activation for " << id_type;
                    }
                    else
                    {
                        if (scheduled_activation < earliest_scheduled_activation)
                        {
                            earliest_scheduled_activation = scheduled_activation;
                        }

                        return;
                    }
                }
                else if (nmos::activation_modes::activate_immediate == staged_mode)
                {
                    // check for cancelled in-flight immediate activation
                    if (nmos::fields::requested_time(staged_activation).is_null()) return;
                    // check for processed in-flight immediate activation
                    if (!nmos::fields::activation_time(staged_activation).is_null()) return;

                    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Processing immediate activation for " << id_type;
                }
                else
                {
                    slog::log<slog::severities::severe>(gate, SLOG_FLF) << "Unexpected activation mode for " << id_type;
                    return;
                }

                const auto activation_time = nmos::tai_now();

                bool active = false;
                nmos::id connected_id;

                // Update the IS-05 connection resource

                nmos::modify_resource(model.connection_resources, resource.id, [&](nmos::resource& connection_resource)
                {
                    const std::pair<nmos::id, nmos::type> id_type{ connection_resource.id, connection_resource.type };
                    auto matching_resource = find_resource(model.node_resources, id_type);
                    if (model.node_resources.end() == matching_resource)
                    {
                        slog::log<slog::severities::error>(gate, SLOG_FLF) << "Implementation error for " << id_type << " during activation: " << "matching IS-04 resource not found";
                        return;
                    }

                    nmos::set_connection_resource_active(connection_resource, [&](web::json::value& endpoint_active)
                    {
                        resolve_auto(*matching_resource, connection_resource, nmos::fields::transport_params(endpoint_active));
                        active = nmos::fields::master_enable(endpoint_active);
                        // Senders indicate the connected receiver_id, receivers indicate the connected sender_id
                        auto& connected_id_or_null = nmos::types::sender == id_type.second ? nmos::fields::receiver_id(endpoint_active) : nmos::fields::sender_id(endpoint_active);
                        if (!connected_id_or_null.is_null()) connected_id = connected_id_or_null.as_string();
                    }, activation_time);

                    set_transportfile(*matching_resource, connection_resource, connection_resource.data[nmos::fields::endpoint_transportfile]);
                });

                // Update the IS-04 resource

                nmos::modify_resource(model.node_resources, resource.id, [&activation_time, &active, &connected_id](nmos::resource& resource)
                {
                    nmos::set_resource_subscription(resource, active, connected_id, activation_time);
                });

                notify = true;
            });

            if ((nmos::tai_clock::time_point::max)() != earliest_scheduled_activation)
            {
                slog::log<slog::severities::more_info>(gate, SLOG_FLF) << "Next scheduled activation is at " << nmos::make_version(nmos::tai_from_time_point(earliest_scheduled_activation))
                    << " in about " << std::fixed << std::setprecision(3) << std::chrono::duration_cast<std::chrono::duration<double>>(earliest_scheduled_activation - now).count() << " seconds time";
            }

            if (notify)
            {
                slog::log<slog::severities::too_much_info>(gate, SLOG_FLF) << "Notifying node behaviour thread"; // and anyone else who cares...
                model.notify();
            }

            most_recent_update = nmos::most_recent_update(model.connection_resources);
        }
    }
}
