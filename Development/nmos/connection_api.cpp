#include "nmos/connection_api.h"

#include "nmos/activation_mode.h"
#include "nmos/api_downgrade.h"
#include "nmos/api_utils.h"
#include "nmos/model.h"
#include "nmos/slog.h"
#include "nmos/thread_utils.h"
#include "nmos/version.h"

namespace nmos
{
    static web::json::value strip_id(const web::json::value &src)
    {
        auto result = src;
        if (result.has_field(nmos::fields::id)) result.erase(nmos::fields::id);
        return result;
    }

    web::http::experimental::listener::api_router make_unmounted_connection_api(nmos::node_model& model, nmos::activate_function activate, slog::base_gate& gate);

    web::http::experimental::listener::api_router make_connection_api(nmos::node_model& model, nmos::activate_function activate, slog::base_gate& gate)
    {
        using namespace web::http::experimental::listener::api_router_using_declarations;

        api_router connection_api;

        connection_api.support(U("/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("x-nmos/" })));
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/x-nmos/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("connection/") }));
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/x-nmos/") + nmos::patterns::connection_api.pattern + U("/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("v1.0/") }));
            return pplx::task_from_result(true);
        });

        connection_api.mount(U("/x-nmos/") + nmos::patterns::connection_api.pattern + U("/") + nmos::patterns::is05_version.pattern, make_unmounted_connection_api(model, activate, gate));

        nmos::add_api_finally_handler(connection_api, gate);

        return connection_api;
    }


    template<typename pair_type, typename rtt>
    static bool patch_key_is_valid(const pair_type &pair, const rtt &resourceType)
    {
        return (pair.first == U("sender_id") && resourceType == U("receivers"))  ||
            (pair.first == U("receiver_id") && resourceType == U("senders"))  ||
            (pair.first == U("transport_file") && resourceType == U("receivers"))  ||
            pair.first == U("activation")  ||
            pair.first == U("master_enable")  ||
            pair.first == U("transport_params");
    }

    web::http::experimental::listener::api_router make_unmounted_connection_api(nmos::node_model& model, nmos::activate_function activate, slog::base_gate& gate)
    {
        using namespace web::http::experimental::listener::api_router_using_declarations;

        api_router connection_api;

        connection_api.support(U("/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("bulk/"), JU("single/") }));
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/bulk/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("senders/"), JU("receivers/") }));
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/bulk/") + nmos::patterns::connectorType.pattern + U("/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::MethodNotAllowed);
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/bulk/") + nmos::patterns::connectorType.pattern + U("/?"), methods::POST, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::NotImplemented);
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/single/?"), methods::GET, [](http_request, http_response res, const string_t&, const route_parameters&)
        {
            set_reply(res, status_codes::OK, value_of({ JU("senders/"), JU("receivers/") }));
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/single/") + nmos::patterns::connectorType.pattern + U("/?"), methods::GET, [&model, &gate](http_request req, http_response res, const string_t&, const route_parameters& parameters)
        {
            auto lock = model.read_lock();
            auto& resources = model.node_resources;

            const string_t resourceType = parameters.at(nmos::patterns::connectorType.name);

            const auto match = [&](const nmos::resources::value_type& resource) { return resource.type == nmos::type_from_resourceType(resourceType) && nmos::is_permitted_downgrade(resource, nmos::is04_versions::v1_2); };

            size_t count = 0;

            set_reply(res, status_codes::OK,
                web::json::serialize_if(resources,
                    match,
                    [&count](const nmos::resources::value_type& resource) { ++count; return value(resource.id + U("/")); }),
                U("application/json"));

            slog::log<slog::severities::info>(gate, SLOG_FLF) << nmos::api_stash(req, parameters) << "Returning " << count << " matching " << resourceType;

            return pplx::task_from_result(true);
        });

        connection_api.support(U("/single/") + nmos::patterns::connectorType.pattern + U("/") + nmos::patterns::resourceId.pattern + U("/?"), methods::GET, [&model, &gate](http_request req, http_response res, const string_t&, const route_parameters& parameters)
        {
            auto lock = model.read_lock();
            auto& resources = model.node_resources;

            const string_t resourceType = parameters.at(nmos::patterns::connectorType.name);
            const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);

            auto resource = find_resource(resources, { resourceId, nmos::type_from_resourceType(resourceType) });
            if (resources.end() != resource && nmos::is_permitted_downgrade(*resource, nmos::is04_versions::v1_2))
            {
                if (nmos::types::sender == resource->type)
                {
                    set_reply(res, status_codes::OK, value_of({ JU("constraints/"), JU("staged/"), JU("active/"), JU("transportfile/") }));
                }
                else // if (nmos::types::receiver == resource->type)
                {
                    set_reply(res, status_codes::OK, value_of({ JU("constraints/"), JU("staged/"), JU("active/") }));
                }
            }
            else
            {
                set_reply(res, status_codes::NotFound);
            }

            return pplx::task_from_result(true);
        });

        connection_api.support(U("/single/") + nmos::patterns::connectorType.pattern + U("/") + nmos::patterns::resourceId.pattern + U("/constraints/?"), methods::GET, [&model, &gate](http_request req, http_response res, const string_t&, const route_parameters& parameters)
        {
            auto lock = model.read_lock();

            const string_t resourceType = parameters.at(nmos::patterns::connectorType.name);
            const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);

            auto &constraints = model.constraints;
            auto resource = find_resource(constraints, {resourceId, nmos::type_from_resourceType(resourceType)});
            if (constraints.end() == resource)
            {
                set_reply(res, status_codes::NotFound);
            }
            else
            {
                // Crude hack here: Since a constraint response is
                // supposed to be an array, but the nmos::resources
                // type expects an object with an "id" field, we
                // presume an "array" field that contains the correct
                // response. The correct way to deal with this, no
                // doubt, is to set the type of model::constraints
                // to something other than nmos::resources.
                set_reply(res, status_codes::OK, resource->data.at(U("array")));
            }

            return pplx::task_from_result(true);
        });

        connection_api.support(U("/single/") + nmos::patterns::connectorType.pattern + U("/") + nmos::patterns::resourceId.pattern + U("/staged/?"), methods::PATCH, [&model, activate, &gate](http_request req, http_response res, const string_t&, const route_parameters& parameters)
        {
            return details::extract_json(req, parameters, gate).then([&, req, res, parameters](value body) mutable
            {
                try
                {
                    const auto& activation = nmos::fields::activation(body);
                    const auto mode = nmos::activation_mode{ nmos::fields::mode(activation) };
                    if (mode != nmos::activation_modes::activate_immediate)
                    {
                        set_reply(res, status_codes::NotImplemented);
                        return true;
                    }
                }
                catch (web::json::json_exception&) {}

                // could start out as a shared/read lock, only upgraded to an exclusive/write lock when the sender/receiver in the resources is actually modified
                auto lock = model.write_lock();

                const string_t resourceType = parameters.at(nmos::patterns::connectorType.name);
                const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);

                auto type = nmos::type_from_resourceType(resourceType);
                auto resource = find_resource(model.staged, { resourceId, type });
                if (model.staged.end() != resource)
                {
                    // First, verify that every key is a valid field.
                    for (const auto & pair: body.as_object())
                    {
                        if (!patch_key_is_valid(pair, resourceType))
                        {
                            set_reply(res, status_codes::BadRequest);
                            return true;
                        }
                    }

                    // OK, now it's safe to update everything.
                    nmos::activation_mode mode;
                    auto update = [&body, &mode] (nmos::resource &resource)
                    {
                        for (const auto& pair: body.as_object())
                        {
                            if (pair.first == U("activation"))
                            {
                                for (const auto &opair: pair.second.as_object())
                                {
                                    if (opair.first == U("mode"))
                                        mode = nmos::activation_mode{ nmos::fields::mode(pair.second) };
                                }
                            }
                            if (pair.first == U("transport_file") || pair.first == U("activation"))
                            {
                                for (const auto &opair: pair.second.as_object())
                                {
                                    resource.data[pair.first][opair.first] = opair.second;
                                }
                                continue;
                            }
                            if (pair.first == U("transport_params"))
                            {
                                const auto& a = pair.second.as_array();
                                for (size_t i = 0; i < std::min<size_t>(2, a.size()); i++)
                                {
                                    for (const auto& tp: a.at(i).as_object())
                                    {
                                        resource.data[pair.first][i][tp.first] = tp.second;
                                    }
                                }
                                continue;
                            }
                            resource.data[pair.first] = pair.second;
                        }
                    };
                    modify_resource(model.patched, resourceId, update);
                    modify_resource(model.staged, resourceId, update);

                    // "If no activation was requested in the PATCH
                    // `activation_time` will be set `null`."
                    // [https://github.com/AMWA-TV/nmos-device-connection-management/blob/v1.0/APIs/ConnectionAPI.raml]
                    auto response = strip_id(resource->data);
                    response[U("activation")][U("activation_time")] = value();

                    if (nmos::activation_modes::activate_immediate == mode)
                    {
                        // should be asynchronous (task-based), with a continuation to return the response
                        {
                            details::reverse_lock_guard<nmos::write_lock> unlock(lock);
                            activate(type, resourceId);
                        }

                        // "For immediate activations on the staged
                        // endpoint this property will be the time the
                        // activation actually occurred in the response
                        // to the PATCH request, but null in response
                        // to any GET requests thereafter."
                        // [https://github.com/AMWA-TV/nmos-device-connection-management/blob/v1.0/APIs/schemas/v1.0-activation-response-schema.json]
                        response[U("activation")][U("activation_time")] = value::string(nmos::make_version());

                        // "For an immediate activation this field will
                        // always be null on the staged endpoint, even
                        // in the response to the PATCH request." [ibid]
                        response[U("activation")][U("requested_time")] = value();

                        auto update = [&body, &mode] (nmos::resource &resource)
                        {
                            auto& activation = resource.data[U("activation")];

                            // "For immediate activations, in the
                            // response to the PATCH request this field
                            // will be set to 'activate_immediate',
                            // but will be null in response to any
                            // subsequent GET requests." [ibid]
                            activation[U("mode")] = value();

                            // "For immediate activations on the staged
                            // endpoint this property will be the time the
                            // activation actually occurred in the response
                            // to the PATCH request, but null in response
                            // to any GET requests thereafter." [ibid]
                            activation[U("activation_time")] = value();

                            // "This field returns to null once the activation
                            // is completed on the staged endpoint." [ibid]
                            activation[U("requested_time")] = value();
                        };
                        modify_resource(model.staged, resourceId, update);

                        modify_resource(model.patched, resourceId, [](nmos::resource &resource)
                        {
                            resource.data = value::object();
                        });
                    }
                    // else pass the buck to a scheduler thread to
                    // deal with the activation at the right time,
                    // and reply with the right HTTP status code.

                    set_reply(res, status_codes::OK, response);
                }
                else
                {
                    set_reply(res, status_codes::NotFound);
                }

                return true;
            });
        });

        connection_api.support(U("/single/") + nmos::patterns::connectorType.pattern + U("/") + nmos::patterns::resourceId.pattern + U("/") + nmos::patterns::stagingType.pattern + U("/?"), methods::GET, [&model, &gate](http_request req, http_response res, const string_t&, const route_parameters& parameters)
        {
            auto lock = model.read_lock();

            const string_t resourceType = parameters.at(nmos::patterns::connectorType.name);
            const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);
            const string_t stagingType = parameters.at(nmos::patterns::stagingType.name);

            auto &resources = stagingType == U("active") ? model.active : model.staged;

            auto resource = find_resource(resources, { resourceId, nmos::type_from_resourceType(resourceType) });
            if (resources.end() == resource)
            {
                set_reply(res, status_codes::NotFound);
            }
            else
            {
                set_reply(res, status_codes::OK, strip_id(resource->data));
            }
            return pplx::task_from_result(true);
        });

        connection_api.support(U("/single/") + nmos::patterns::senderType.pattern + U("/") + nmos::patterns::resourceId.pattern + U("/transportfile/?"), methods::GET, [&model, &gate](http_request req, http_response res, const string_t&, const route_parameters& parameters)
        {
            auto lock = model.read_lock();

            const string_t resourceId = parameters.at(nmos::patterns::resourceId.name);
            auto& redirects = model.transportfile.redirects;
            auto url = redirects.find(resourceId);
            if (url != redirects.end())
            {
                set_reply(res, status_codes::TemporaryRedirect);
                res.headers().add(web::http::header_names::location, url->second);
            }
            else
            {
                set_reply(res, status_codes::NotFound);
            }
            return pplx::task_from_result(true);
        });

        return connection_api;
    }
}
