//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/app/main/Application.h>
#include <ripple/json/json_reader.h>
#include <ripple/server/JsonWriter.h>
#include <ripple/server/make_ServerHandler.h>
#include <ripple/server/impl/JSONRPCUtil.h>
#include <ripple/server/impl/ServerHandlerImp.h>
#include <ripple/basics/contract.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/make_SSLContext.h>
#include <ripple/core/JobQueue.h>
#include <ripple/json/to_string.h>
#include <ripple/server/make_Server.h>
#include <ripple/overlay/Overlay.h>
#include <ripple/resource/ResourceManager.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/impl/Tuning.h>
#include <beast/crypto/base64.h>
#include <ripple/rpc/RPCHandler.h>
#include <beast/http/rfc2616.h>
#include <boost/algorithm/string.hpp>
#include <boost/type_traits.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>
#include <algorithm>
#include <stdexcept>

namespace ripple {

ServerHandler::ServerHandler (Stoppable& parent)
    : Stoppable ("ServerHandler", parent)
{
}

//------------------------------------------------------------------------------

ServerHandlerImp::ServerHandlerImp (Application& app, Stoppable& parent,
    boost::asio::io_service& io_service, JobQueue& jobQueue,
        NetworkOPs& networkOPs, Resource::Manager& resourceManager,
            CollectorManager& cm)
    : ServerHandler (parent)
    , app_ (app)
    , m_resourceManager (resourceManager)
    , m_journal (app_.journal("Server"))
    , m_networkOPs (networkOPs)
    , m_server (make_Server(
        *this, io_service, app_.journal("Server")))
    , m_jobQueue (jobQueue)
{
    auto const& group (cm.group ("rpc"));
    rpc_requests_ = group->make_counter ("requests");
    rpc_size_ = group->make_event ("size");
    rpc_time_ = group->make_event ("time");
}

ServerHandlerImp::~ServerHandlerImp()
{
    m_server = nullptr;
}

void
ServerHandlerImp::setup (Setup const& setup, beast::Journal journal)
{
    setup_ = setup;
    m_server->ports (setup.ports);
}

//------------------------------------------------------------------------------

void
ServerHandlerImp::onStop()
{
    m_server->close();
}

//------------------------------------------------------------------------------

bool
ServerHandlerImp::onAccept (Session& session,
    boost::asio::ip::tcp::endpoint endpoint)
{
    std::lock_guard<std::mutex> l(countlock_);

    auto const c = ++count_[session.port()];

    if (session.port().limit && c >= session.port().limit)
    {
        JLOG (m_journal.trace) <<
            session.port().name << " is full; dropping " <<
            endpoint;
        return false;
    }

    return true;
}

auto
ServerHandlerImp::onHandoff (Session& session,
    std::unique_ptr <beast::asio::ssl_bundle>&& bundle,
        beast::http::message&& request,
            boost::asio::ip::tcp::endpoint remote_address) ->
    Handoff
{
    if (session.port().protocol.count("wss") > 0 &&
        isWebsocketUpgrade (request))
    {
        // Pass to websockets
        Handoff handoff;
        // handoff.moved = true;
        return handoff;
    }
    if (session.port().protocol.count("peer") > 0)
        return app_.overlay().onHandoff (std::move(bundle),
            std::move(request), remote_address);
    // Pass through to legacy onRequest
    return Handoff{};
}

auto
ServerHandlerImp::onHandoff (Session& session,
    boost::asio::ip::tcp::socket&& socket,
        beast::http::message&& request,
            boost::asio::ip::tcp::endpoint remote_address) ->
    Handoff
{
    if (session.port().protocol.count("ws") > 0 &&
        isWebsocketUpgrade (request))
    {
        // Pass to websockets
        Handoff handoff;
        // handoff.moved = true;
        return handoff;
    }
    // Pass through to legacy onRequest
    return Handoff{};
}

static inline
Json::Output makeOutput (Session& session)
{
    return [&](boost::string_ref const& b)
    {
        session.write (b.data(), b.size());
    };
}

void
ServerHandlerImp::onRequest (Session& session)
{
    // Make sure RPC is enabled on the port
    if (session.port().protocol.count("http") == 0 &&
        session.port().protocol.count("https") == 0)
    {
        HTTPReply (403, "Forbidden", makeOutput (session), app_.journal ("RPC"));
        session.close (true);
        return;
    }

    // Check user/password authorization
    if (! authorized (
            session.port(), build_map(session.request().headers)))
    {
        HTTPReply (403, "Forbidden", makeOutput (session), app_.journal ("RPC"));
        session.close (true);
        return;
    }

    m_jobQueue.postCoro(jtCLIENT, "RPC-Client",
        [this, detach = session.detach()](std::shared_ptr<JobCoro> jc)
        {
            processSession(detach, jc);
        });
}

void
ServerHandlerImp::onClose (Session& session,
    boost::system::error_code const&)
{
    std::lock_guard<std::mutex> l(countlock_);
    --count_[session.port()];
}

void
ServerHandlerImp::onStopped (Server&)
{
    stopped();
}

//------------------------------------------------------------------------------

// Run as a couroutine.
void
ServerHandlerImp::processSession (std::shared_ptr<Session> const& session,
    std::shared_ptr<JobCoro> jobCoro)
{
    processRequest (session->port(), to_string (session->body()),
        session->remoteAddress().at_port (0), makeOutput (*session), jobCoro,
        [&]
        {
            auto const iter =
                session->request().headers.find(
                    "X-Forwarded-For");
            if(iter != session->request().headers.end())
                return iter->second;
            return std::string{};
        }(),
        [&]
        {
            auto const iter =
                session->request().headers.find(
                    "X-User");
            if(iter != session->request().headers.end())
                return iter->second;
            return std::string{};
        }());

    if (session->request().keep_alive())
        session->complete();
    else
        session->close (true);
}

void
ServerHandlerImp::processRequest (Port const& port,
    std::string const& request, beast::IP::Endpoint const& remoteIPAddress,
        Output&& output, std::shared_ptr<JobCoro> jobCoro,
        std::string forwardedFor, std::string user)
{
    auto rpcJ = app_.journal ("RPC");

    Json::Value jsonRPC;
    {
        Json::Reader reader;
        if ((request.size () > RPC::Tuning::maxRequestSize) ||
            ! reader.parse (request, jsonRPC) ||
            ! jsonRPC ||
            ! jsonRPC.isObject ())
        {
            HTTPReply (400, "Unable to parse request", output, rpcJ);
            return;
        }
    }

    // Parse id now so errors from here on will have the id
    //
    // VFALCO NOTE Except that "id" isn't included in the following errors.
    //
    Json::Value const& id = jsonRPC ["id"];
    Json::Value const& method = jsonRPC ["method"];

    if (! method) {
        HTTPReply (400, "Null method", output, rpcJ);
        return;
    }

    if (!method.isString ()) {
        HTTPReply (400, "method is not string", output, rpcJ);
        return;
    }

    /* ---------------------------------------------------------------------- */
    auto role = Role::FORBID;
    auto required = RPC::roleRequired(id.asString());

    if (jsonRPC.isObject() && jsonRPC.isMember("params") &&
            jsonRPC["params"].isArray() && jsonRPC["params"].size() > 0 &&
                jsonRPC["params"][Json::UInt(0)].isObject())
    {
        role = requestRole(required, port, jsonRPC["params"][Json::UInt(0)],
            remoteIPAddress, user);
    }
    else
    {
        role = requestRole(required, port, Json::objectValue,
            remoteIPAddress, user);
    }

    /**
     * Clear header-assigned values if not positively identified from a
     * secure_gateway.
     */
    if (role != Role::IDENTIFIED)
    {
        forwardedFor.clear();
        user.clear();
    }

    Resource::Consumer usage;

    if (isUnlimited (role))
        usage = m_resourceManager.newUnlimitedEndpoint (
            remoteIPAddress.to_string());
    else
        usage = m_resourceManager.newInboundEndpoint(remoteIPAddress);

    if (usage.disconnect ())
    {
        HTTPReply (503, "Server is overloaded", output, rpcJ);
        return;
    }

    std::string strMethod = method.asString ();
    if (strMethod.empty())
    {
        HTTPReply (400, "method is empty", output, rpcJ);
        return;
    }

    // Extract request parameters from the request Json as `params`.
    //
    // If the field "params" is empty, `params` is an empty object.
    //
    // Otherwise, that field must be an array of length 1 (why?)
    // and we take that first entry and validate that it's an object.
    Json::Value params = jsonRPC [jss::params];

    if (! params)
        params = Json::Value (Json::objectValue);

    else if (!params.isArray () || params.size() != 1)
    {
        HTTPReply (400, "params unparseable", output, rpcJ);
        return;
    }
    else
    {
        params = std::move (params[0u]);
        if (!params.isObject())
        {
            HTTPReply (400, "params unparseable", output, rpcJ);
            return;
        }
    }

    // VFALCO TODO Shouldn't we handle this earlier?
    //
    if (role == Role::FORBID)
    {
        // VFALCO TODO Needs implementing
        // FIXME Needs implementing
        // XXX This needs rate limiting to prevent brute forcing password.
        HTTPReply (403, "Forbidden", output, rpcJ);
        return;
    }

    Resource::Charge loadType = Resource::feeReferenceRPC;

    JLOG(m_journal.debug) << "Query: " << strMethod << params;

    // Provide the JSON-RPC method as the field "command" in the request.
    params[jss::command] = strMethod;
    JLOG (m_journal.trace)
        << "doRpcCommand:" << strMethod << ":" << params;

    auto const start (std::chrono::high_resolution_clock::now ());

    RPC::Context context {m_journal, params, app_, loadType, m_networkOPs,
        app_.getLedgerMaster(), role, jobCoro, InfoSub::pointer(),
        {user, forwardedFor}};
    Json::Value result;
    RPC::doCommand (context, result);

    // Always report "status".  On an error report the request as received.
    if (result.isMember (jss::error))
    {
        result[jss::status] = jss::error;
        result[jss::request] = params;
        JLOG (m_journal.debug)  <<
            "rpcError: " << result [jss::error] <<
            ": " << result [jss::error_message];
    }
    else
    {
        result[jss::status]  = jss::success;
    }

    Json::Value reply (Json::objectValue);
    reply[jss::result] = std::move (result);
    auto response = to_string (reply);

    rpc_time_.notify (static_cast <beast::insight::Event::value_type> (
        std::chrono::duration_cast <std::chrono::milliseconds> (
            std::chrono::high_resolution_clock::now () - start)));
    ++rpc_requests_;
    rpc_size_.notify (static_cast <beast::insight::Event::value_type> (
        response.size ()));

    response += '\n';
    usage.charge (loadType);

    if (m_journal.debug.active())
    {
        static const int maxSize = 10000;
        if (response.size() <= maxSize)
            m_journal.debug << "Reply: " << response;
        else
            m_journal.debug << "Reply: " << response.substr (0, maxSize);
    }

    HTTPReply (200, response, output, rpcJ);
}

//------------------------------------------------------------------------------

// Returns `true` if the HTTP request is a Websockets Upgrade
// http://en.wikipedia.org/wiki/HTTP/1.1_Upgrade_header#Use_with_WebSockets
bool
ServerHandlerImp::isWebsocketUpgrade (beast::http::message const& request)
{
    if (request.upgrade())
        return request.headers["Upgrade"] == "websocket";
    return false;
}

// VFALCO TODO Rewrite to use beast::http::headers
bool
ServerHandlerImp::authorized (Port const& port,
    std::map<std::string, std::string> const& h)
{
    if (port.user.empty() || port.password.empty())
        return true;

    auto const it = h.find ("authorization");
    if ((it == h.end ()) || (it->second.substr (0, 6) != "Basic "))
        return false;
    std::string strUserPass64 = it->second.substr (6);
    boost::trim (strUserPass64);
    std::string strUserPass = beast::base64_decode (strUserPass64);
    std::string::size_type nColon = strUserPass.find (":");
    if (nColon == std::string::npos)
        return false;
    std::string strUser = strUserPass.substr (0, nColon);
    std::string strPassword = strUserPass.substr (nColon + 1);
    return strUser == port.user && strPassword == port.password;
}

//------------------------------------------------------------------------------

void
ServerHandler::appendStandardFields (beast::http::message& message)
{
}

//------------------------------------------------------------------------------

void
ServerHandler::Setup::makeContexts()
{
    for(auto& p : ports)
    {
        if (p.secure())
        {
            if (p.ssl_key.empty() && p.ssl_cert.empty() &&
                    p.ssl_chain.empty())
                p.context = make_SSLContext();
            else
                p.context = make_SSLContextAuthed (
                    p.ssl_key, p.ssl_cert, p.ssl_chain);
        }
        else
        {
            p.context = std::make_shared<
                boost::asio::ssl::context>(
                    boost::asio::ssl::context::sslv23);
        }
    }
}

static
Port
to_Port(ParsedPort const& parsed, std::ostream& log)
{
    Port p;
    p.name = parsed.name;

    if (! parsed.ip)
    {
        log << "Missing 'ip' in [" << p.name << "]\n";
        Throw<std::exception> ();
    }
    p.ip = *parsed.ip;

    if (! parsed.port)
    {
        log << "Missing 'port' in [" << p.name << "]\n";
        Throw<std::exception> ();
    }
    else if (*parsed.port == 0)
    {
        log << "Port " << *parsed.port << "in [" << p.name << "] is invalid\n";
        Throw<std::exception> ();
    }
    p.port = *parsed.port;
    if (parsed.admin_ip)
        p.admin_ip = *parsed.admin_ip;
    if (parsed.secure_gateway_ip)
        p.secure_gateway_ip = *parsed.secure_gateway_ip;

    if (parsed.protocol.empty())
    {
        log << "Missing 'protocol' in [" << p.name << "]\n";
        Throw<std::exception> ();
    }
    p.protocol = parsed.protocol;
    if (p.websockets() &&
        (parsed.protocol.count("peer") > 0 ||
        parsed.protocol.count("http") > 0 ||
        parsed.protocol.count("https") > 0))
    {
        log << "Invalid protocol combination in [" << p.name << "]\n";
        Throw<std::exception> ();
    }

    p.user = parsed.user;
    p.password = parsed.password;
    p.admin_user = parsed.admin_user;
    p.admin_password = parsed.admin_password;
    p.ssl_key = parsed.ssl_key;
    p.ssl_cert = parsed.ssl_cert;
    p.ssl_chain = parsed.ssl_chain;

    return p;
}

static
std::vector<Port>
parse_Ports (
    Config const& config,
    std::ostream& log)
{
    std::vector<Port> result;

    if (! config.exists("server"))
    {
        log <<
            "Required section [server] is missing\n";
        Throw<std::exception> ();
    }

    ParsedPort common;
    parse_Port (common, config["server"], log);

    auto const& names = config.section("server").values();
    result.reserve(names.size());
    for (auto const& name : names)
    {
        if (! config.exists(name))
        {
            log <<
                "Missing section: [" << name << "]\n";
            Throw<std::exception> ();
        }
        ParsedPort parsed = common;
        parsed.name = name;
        parse_Port(parsed, config[name], log);
        result.push_back(to_Port(parsed, log));
    }

    if (config.RUN_STANDALONE)
    {
        auto it = result.begin ();

        while (it != result.end())
        {
            auto& p = it->protocol;

            // Remove the peer protocol, and if that would
            // leave the port empty, remove the port as well
            if (p.erase ("peer") && p.empty())
                it = result.erase (it);
            else
                ++it;
        }
    }
    else
    {
        auto const count = std::count_if (
            result.cbegin(), result.cend(),
            [](Port const& p)
            {
                return p.protocol.count("peer") != 0;
            });

        if (count > 1)
        {
            log << "Error: More than one peer protocol configured in [server]\n";
            Throw<std::exception> ();
        }

        if (count == 0)
            log << "Warning: No peer protocol configured\n";
    }

    return result;
}

// Fill out the client portion of the Setup
static
void
setup_Client (ServerHandler::Setup& setup)
{
    decltype(setup.ports)::const_iterator iter;
    for (iter = setup.ports.cbegin();
            iter != setup.ports.cend(); ++iter)
        if (iter->protocol.count("http") > 0 ||
                iter->protocol.count("https") > 0)
            break;
    if (iter == setup.ports.cend())
        return;
    setup.client.secure =
        iter->protocol.count("https") > 0;
    setup.client.ip = iter->ip.to_string();
    // VFALCO HACK! to make localhost work
    if (setup.client.ip == "0.0.0.0")
        setup.client.ip = "127.0.0.1";
    setup.client.port = iter->port;
    setup.client.user = iter->user;
    setup.client.password = iter->password;
    setup.client.admin_user = iter->admin_user;
    setup.client.admin_password = iter->admin_password;
}

// Fill out the overlay portion of the Setup
static
void
setup_Overlay (ServerHandler::Setup& setup)
{
    auto const iter = std::find_if(
        setup.ports.cbegin(), setup.ports.cend(),
        [](Port const& port)
        {
            return port.protocol.count("peer") != 0;
        });
    if (iter == setup.ports.cend())
    {
        setup.overlay.port = 0;
        return;
    }
    setup.overlay.ip = iter->ip;
    setup.overlay.port = iter->port;
}

ServerHandler::Setup
setup_ServerHandler(
    Config const& config,
    std::ostream& log)
{
    ServerHandler::Setup setup;
    setup.ports = parse_Ports(config, log);

    setup_Client(setup);
    setup_Overlay(setup);

    return setup;
}

std::unique_ptr <ServerHandler>
make_ServerHandler (Application& app, beast::Stoppable& parent,
    boost::asio::io_service& io_service, JobQueue& jobQueue,
        NetworkOPs& networkOPs, Resource::Manager& resourceManager,
            CollectorManager& cm)
{
    return std::make_unique<ServerHandlerImp>(app, parent,
        io_service, jobQueue, networkOPs, resourceManager, cm);
}

} // ripple
