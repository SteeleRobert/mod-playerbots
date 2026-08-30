/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LlmClient.h"

#include "Log.h"
#include "PlayerbotAIConfig.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    std::mutex g_replyMutex;
    std::vector<LlmReply> g_replies;
    std::atomic<uint32> g_inFlight{0};
    std::once_flag g_curlInit;

    size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        std::string* buffer = static_cast<std::string*>(userp);
        size_t const total = size * nmemb;
        buffer->append(static_cast<char*>(contents), total);
        return total;
    }

    // Ollama with "stream": false answers with a single JSON object, but the
    // shim/proxy in front of it may still emit NDJSON. Accept both: parse the whole
    // body first, and fall back to concatenating the "response" field of each line.
    // OpenAI-compatible (vLLM): the answer is choices[0].message.content, and a
    // reasoning model puts its scratchpad in choices[0].message.reasoning_content -
    // the exact analogue of Ollama's "thinking", with the same failure mode of an
    // empty content field. /v1/completions style (choices[0].text) is accepted too.
    std::string ExtractOpenAiText(std::string const& body, std::string& error)
    {
        try
        {
            nlohmann::json doc = nlohmann::json::parse(body);

            if (doc.contains("error"))
            {
                nlohmann::json const& err = doc["error"];
                error = err.is_string() ? err.get<std::string>()
                      : (err.is_object() && err.contains("message") && err["message"].is_string())
                            ? err["message"].get<std::string>()
                            : err.dump();
                return "";
            }

            if (!doc.contains("choices") || !doc["choices"].is_array() || doc["choices"].empty())
            {
                error = "no choices in reply";
                return "";
            }

            nlohmann::json const& choice = doc["choices"][0];

            if (choice.contains("message") && choice["message"].is_object())
            {
                nlohmann::json const& message = choice["message"];
                for (char const* key : {"content", "reasoning_content"})
                {
                    if (message.contains(key) && message[key].is_string())
                    {
                        std::string const text = message[key].get<std::string>();
                        if (!text.empty())
                            return text;
                    }
                }
            }

            if (choice.contains("text") && choice["text"].is_string())
            {
                std::string const text = choice["text"].get<std::string>();
                if (!text.empty())
                    return text;
            }

            error = "reply carried no content";
            return "";
        }
        catch (std::exception const& e)
        {
            error = std::string("could not parse reply envelope: ") + e.what();
            return "";
        }
    }

    std::string ExtractResponseText(std::string const& body, std::string& error)
    {
        auto pull = [&](nlohmann::json const& doc, std::string& out) -> bool
        {
            if (!doc.is_object())
                return false;
            if (doc.contains("error"))
            {
                error = doc["error"].is_string() ? doc["error"].get<std::string>() : doc["error"].dump();
                return false;
            }
            if (doc.contains("response") && doc["response"].is_string())
            {
                std::string const text = doc["response"].get<std::string>();
                if (!text.empty())
                {
                    out += text;
                    return true;
                }
            }
            // Empty "response" plus a populated "thinking" means the endpoint
            // ignored think=false. The answer is in there; take it rather than
            // reporting an empty reply.
            if (doc.contains("thinking") && doc["thinking"].is_string())
            {
                std::string const text = doc["thinking"].get<std::string>();
                if (!text.empty())
                {
                    out += text;
                    return true;
                }
            }
            return false;
        };

        std::string out;
        try
        {
            nlohmann::json doc = nlohmann::json::parse(body);
            if (pull(doc, out))
                return out;
        }
        catch (std::exception const&)
        {
            // fall through to the NDJSON path
        }

        if (!error.empty())
            return "";

        std::istringstream stream(body);
        std::string line;
        bool any = false;
        while (std::getline(stream, line))
        {
            if (line.empty())
                continue;
            try
            {
                nlohmann::json doc = nlohmann::json::parse(line);
                any |= pull(doc, out);
            }
            catch (std::exception const&)
            {
            }
        }

        if (!any && error.empty())
            error = "endpoint returned no 'response' field";
        return out;
    }

    // One HTTP POST. Shared by both API shapes so the transport, timeout and error
    // handling cannot drift apart between them.
    std::string PerformPost(std::string const& payload, std::string& error, bool openAi)
    {
        std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            error = "curl_easy_init failed";
            return "";
        }

        curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, sPlayerbotAIConfig.llmDirectiveUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(sPlayerbotAIConfig.llmDirectiveTimeoutSeconds));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode const res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            error = std::string("transport: ") + curl_easy_strerror(res);
            return "";
        }
        if (httpCode < 200 || httpCode >= 300)
        {
            // The body usually explains a 4xx far better than the status alone.
            error = "http " + std::to_string(httpCode) + ": " + body.substr(0, 200);
            return "";
        }

        return openAi ? ExtractOpenAiText(body, error) : ExtractResponseText(body, error);
    }

    std::string PerformRequest(std::string const& prompt, std::string& error)
    {
        std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            error = "curl_easy_init failed";
            return "";
        }

        // HARD-WON: passing a JSON *schema* in `format` is silently ignored by the
        // MLX runner - it answers in prose and every reply is discarded. Plain
        // "json" is honoured by every runner we have measured, so the shape is
        // pinned in the prompt text instead and `format` stays a bare "json".
        bool const openAi = sPlayerbotAIConfig.llmDirectiveOpenAiApi;

        nlohmann::json request;
        if (openAi)
        {
            request = {
                {"model", sPlayerbotAIConfig.llmDirectiveModel},
                {"messages", nlohmann::json::array({
                    {{"role", "user"}, {"content", prompt}}
                })},
                {"stream", false},
                // vLLM honours this; it is the OpenAI-side equivalent of Ollama's
                // format:"json". The shape still goes in the prompt text as well -
                // a *schema* is silently ignored by several runners, so it is never
                // the only thing holding the output together.
                {"response_format", {{"type", "json_object"}}},
                {"max_tokens", sPlayerbotAIConfig.llmDirectiveNumPredict},
                {"temperature", sPlayerbotAIConfig.llmDirectiveTemperature}
            };

            // Qwen-family chat templates take this; it is how thinking is turned off
            // on the vLLM side, where there is no top-level "think" flag.
            if (sPlayerbotAIConfig.llmDirectiveDisableThinking)
                request["chat_template_kwargs"] = {{"enable_thinking", false}};

            std::string const payloadOpenAi = request.dump();
            return PerformPost(payloadOpenAi, error, /*openAi*/ true);
        }

        request = {
            {"model", sPlayerbotAIConfig.llmDirectiveModel},
            {"prompt", prompt},
            {"stream", false},
            {"format", "json"},
            {"options", {
                // HARD-WON: a sibling feature shipped with num_predict ~128 and 97%
                // of its replies were cut off mid-object. Size this to the reply you
                // actually want, and treat a short/unparseable reply as a failure.
                {"num_predict", sPlayerbotAIConfig.llmDirectiveNumPredict},
                {"temperature", sPlayerbotAIConfig.llmDirectiveTemperature}
            }}
        };
        // MEASURED: a reasoning model (qwen3.5) puts its answer in "thinking" and
        // returns an EMPTY "response", which is indistinguishable downstream from
        // the endpoint saying nothing at all. Asking it not to think is the fix;
        // ExtractResponseText also falls back to "thinking" for endpoints that
        // ignore this flag.
        if (sPlayerbotAIConfig.llmDirectiveDisableThinking)
            request["think"] = false;

        std::string const payload = request.dump();
        return PerformPost(payload, error, /*openAi*/ false);
    }
}

namespace LlmClient
{
    uint32 InFlight() { return g_inFlight.load(); }

    bool Dispatch(ObjectGuid guid, std::string const& agentId, std::string const& prompt)
    {
        uint32 const cap = sPlayerbotAIConfig.llmDirectiveMaxConcurrent;
        if (cap && g_inFlight.load() >= cap)
            return false;

        ++g_inFlight;

        std::string promptCopy = prompt;
        std::string agentIdCopy = agentId;
        std::thread([guid, agentIdCopy, promptCopy]()
        {
            auto const started = std::chrono::steady_clock::now();

            LlmReply reply;
            reply.guid = guid;
            reply.agentId = agentIdCopy;
            reply.prompt = promptCopy;
            reply.raw = PerformRequest(promptCopy, reply.error);
            reply.latencyMs = static_cast<uint32>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count());

            {
                std::lock_guard<std::mutex> lock(g_replyMutex);
                g_replies.push_back(std::move(reply));
            }
            --g_inFlight;
        }).detach();

        return true;
    }

    bool TakeReply(ObjectGuid guid, std::string const& agentId, LlmReply& out)
    {
        std::lock_guard<std::mutex> lock(g_replyMutex);
        for (size_t i = 0; i < g_replies.size(); ++i)
        {
            if (g_replies[i].guid != guid || g_replies[i].agentId != agentId)
                continue;
            out = std::move(g_replies[i]);
            g_replies.erase(g_replies.begin() + i);
            return true;
        }
        return false;
    }

    void DropPending(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_replyMutex);
        for (size_t i = 0; i < g_replies.size();)
        {
            if (g_replies[i].guid == guid)
                g_replies.erase(g_replies.begin() + i);
            else
                ++i;
        }
    }
}
