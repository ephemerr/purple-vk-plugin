#include <debug.h>
#include <server.h>
#include <util.h>

#include "vk-captcha.h"
#include "vk-common.h"
#include "vk-message.h"
#include "utils.h"

#include "vk-api.h"

namespace
{

// Helper struct used to reduce length of function signatures.
struct SendMessage
{
    string uid;
    string message;
    SendSuccessCb success_cb;
    ErrorCb error_cb;
};

// Helper function, used in send_im_message and request_captcha.
void send_im_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid = "",
                              const string& captcha_key = "");

} // End of anonymous namespace

int send_im_message(PurpleConnection* gc, const char* uid, const char* message,
                    const SendSuccessCb& success_cb, const ErrorCb& error_cb)
{
    // NOTE: We de-HTMLify message before sending, because
    //  * Vk.com chat is plaintext anyway
    //  * Vk.com accepts '\n' in place of <br>
    char* unescaped_message = purple_unescape_html(message);
    send_im_message_internal(gc, { uid, unescaped_message, success_cb, error_cb });
    g_free(unescaped_message);
    return 1;
}

namespace
{

// Process error and call either success_cb or error_cb. The only error which is meaningfully
// processed is CAPTCHA request.
void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message);

void send_im_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid,
                              const string& captcha_key)
{
    CallParams params = { {"user_id", message.uid}, {"message", message.message}, {"type", "1"} };
    if (!captcha_sid.empty())
        params.push_back(make_pair("captcha_sid", captcha_sid));
    if (!captcha_key.empty())
        params.push_back(make_pair("captcha_key", captcha_key));
    vk_call_api(gc, "messages.send", params, [=](const picojson::value&) {
        if (message.success_cb)
            message.success_cb();
    }, [=](const picojson::value& error) {
        process_im_error(error, gc, message);
    });
}

// Add error message to debug log, message window and call error_cb
void show_error(PurpleConnection* gc, const string& uid, const SendMessage& message);

PurpleConversation* find_conv_for_uid(PurpleConnection* gc, const string& uid)
{
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, ("id" + uid).c_str(),
                                                 purple_connection_get_account(gc));
}

void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message)
{
    if (!error.is<picojson::object>() || !field_is_present<double>(error, "error_code")) {
        // Most probably, network timeout.
        show_error(gc, message.uid, message);
        return;
    }
    int error_code = error.get("error_code").get<double>();
    if (error_code != VK_CAPTCHA_NEEDED) {
        show_error(gc, message.uid, message);
        return;
    }
    if (!field_is_present<string>(error, "captcha_sid") || !field_is_present<string>(error, "captcha_img")) {
        purple_debug_error("prpl-vkcom", "Captcha request does not contain captcha_sid or captcha_img");
        show_error(gc, message.uid, message);
        return;
    }

    const string& captcha_sid = error.get("captcha_sid").get<string>();
    const string& captcha_img = error.get("captcha_img").get<string>();
    purple_debug_info("prpl-vkcom", "Received CAPTCHA %s\n", captcha_img.c_str());

    request_captcha(gc, captcha_img, [=](const string& captcha_key) {
        send_im_message_internal(gc, message, captcha_sid, captcha_key);
    }, [=] {
        show_error(gc, message.uid, message);
    });
}

void show_error(PurpleConnection* gc, const string& uid, const SendMessage& message)
{
    purple_debug_error("prpl-vkcom", "Error sending message to %s: %s\n", message.uid.c_str(), message.message.c_str());

    PurpleConversation* conv = find_conv_for_uid(gc, uid);
    if (conv) {
        char* escaped_message = g_markup_escape_text(message.message.c_str(), -1);
        string error_msg = str_format("Error sending message '%s'", escaped_message);
        purple_conversation_write(conv, nullptr, error_msg.c_str(),
                                  PurpleMessageFlags(PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_NO_LINKIFY), time(nullptr));
        g_free(escaped_message);
    }

    if (message.error_cb)
        message.error_cb();
}

} // End of anonymous namespace


unsigned send_typing_notification(PurpleConnection* gc, const char* uid)
{
    CallParams params = { {"user_id", uid}, {"type", "typing"} };
    vk_call_api(gc, "messages.setActivity", params);

    // Resend typing notification in 5 seconds
    return 5;
}


namespace
{

// Creates string of integers, separated by sep.
template<typename Sep, typename It>
string str_concat_int(Sep sep, It first, It last)
{
    string s;
    for (It it = first; it != last; it++) {
        if (!s.empty())
            s += sep;
        char buf[128];
        sprintf(buf, "%lld", (long long)*it);
        s += buf;
    }
    return s;
}

} // End of anonymous namespace

void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids)
{
    // Creates string of identifiers, separated with comma.
    string ids_str = str_concat_int(',', message_ids.begin(), message_ids.end());

    CallParams params = { {"message_ids", ids_str} };
    vk_call_api(gc, "messages.markAsRead", params, [=] (const picojson::value&) {});
}

namespace
{

// Two reasons for creating a separate class:
//  a) messages.get returns answers in reverse time order, so we have to store messages and sort them later;
//  b) messages.get paginates the answers, so that multiple calls may be required in order to retrieve all
//     messages.
// NOTE: This design (along with VkAuthenticator in vk-auth.cpp) seems to bee too heavyweight and Java-ish,
// but I cannot create any better. Any ideas?

class MessageReceiver
{
public:
    static MessageReceiver* create(PurpleConnection* gc, const FinishedCb& finished_cb);

    void run();

private:
    struct ReceivedMessage
    {
        uint64_t uid;
        uint64_t mid;
        string text;
        uint64_t timestamp;
    };
    vector<ReceivedMessage> m_messages;

    PurpleConnection* m_gc;
    FinishedCb m_finished_cb;

    MessageReceiver(PurpleConnection* gc, const FinishedCb& finished_cb)
        : m_gc(gc),
          m_finished_cb(finished_cb)
    {
    }

    void receive(int offset);
    void finish();
};

} // End of anonymous namespace

void receive_unread_messages(PurpleConnection* gc, const FinishedCb& finished_cb)
{
    MessageReceiver* receiver = MessageReceiver::create(gc, finished_cb);
    receiver->run();
}

namespace
{

MessageReceiver* MessageReceiver::create(PurpleConnection* gc, const FinishedCb& finished_cb)
{
    return new MessageReceiver(gc, finished_cb);
}

void MessageReceiver::run()
{
    receive(0);
}

void MessageReceiver::receive(int offset)
{
    CallParams params = { {"out", "0"}, {"filters", "1"}, {"offset", str_format("%d", offset)} };
    vk_call_api(m_gc, "messages.get", params, [=] (const picojson::value& result) {
        if (!field_is_present<double>(result, "count") || !field_is_present<picojson::array>(result, "items")) {
            purple_debug_error("prpl-vkcom", "Strange response to messages.get: %s\n", result.serialize().c_str());
            finish();
            return;
        }

        const picojson::array& items = result.get("items").get<picojson::array>();
        // We ignore "count", simply increasing offset until we receive empty list.
        if (items.size() == 0) {
            finish();
            return;
        }

        for (const picojson::value& v: items) {
            if (!field_is_present<double>(v, "user_id") || !field_is_present<double>(v, "date")
                    || !field_is_present<string>(v, "body") || !field_is_present<double>(v, "id")) {
                purple_debug_error("prpl-vkcom", "Strange response to messages.get: %s\n", result.serialize().c_str());
                finish();
                return;
            }

            uint64_t uid = v.get("user_id").get<double>();
            uint64_t mid = v.get("id").get<double>();
            const string& text = v.get("body").get<string>();
            uint64_t timestamp = v.get("date").get<double>();

            m_messages.push_back({ uid, mid, text, timestamp });
        }
        receive(offset + items.size());
    }, [=] (const picojson::value&) {
        finish();
    });
}

void MessageReceiver::finish()
{
    std::sort(m_messages.begin(), m_messages.end(), [](const ReceivedMessage& a, const ReceivedMessage& b) {
        return a.timestamp < b.timestamp;
    });

    uint64_vec message_ids;
    for (const ReceivedMessage& m: m_messages) {
        char name[128];
        sprintf(name, "id%llu", (long long)m.uid);
        serv_got_im(m_gc, name, m.text.c_str(), PURPLE_MESSAGE_RECV, m.timestamp);
        message_ids.push_back(m.mid);
    }
    mark_message_as_read(m_gc, message_ids);

    if (m_finished_cb)
        m_finished_cb();
    delete this;
}

} // End of anonymous namespace
