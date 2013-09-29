#include <debug.h>
#include <imgstore.h>
#include <server.h>
#include <util.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"

#include "vk-message-recv.h"


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

template<typename Sep, typename C>
string str_concat_int(Sep sep, const C& c)
{
    return str_concat_int(sep, c.cbegin(), c.cend());
}

// Three reasons for creating a separate class:
//  a) messages.get returns answers in reverse time order, so we have to store messages and sort them later;
//  b) messages.get paginates the answers, so that multiple calls may be required in order to retrieve all
//     messages;
//  c) we have to run a bunch of HTTP requests to retrieve photo and video thumbnails and append them to
//     received messages.
// NOTE: This design (along with VkAuthenticator in vk-auth.cpp) seems to bee too heavyweight and Java-ish,
// but I cannot create any better. Any ideas?

class MessageReceiver
{
public:
    static MessageReceiver* create(PurpleConnection* gc, const ReceivedCb& recevied_cb);

    // Receives all unread messages.
    void run_unread();
    // Receives messages with given ids.
    void run(const uint64_vec& message_ids);

private:
    struct ReceivedMessage
    {
        uint64 uid;
        uint64 mid;
        string text;
        uint64 timestamp;

        // A list of thumbnail URLs to download and append to message. Set in process_attachments,
        // used in download_thumbnails.
        vector<string> thumbnail_urls;
    };
    vector<ReceivedMessage> m_messages;

    PurpleConnection* m_gc;
    ReceivedCb m_received_cb;

    MessageReceiver(PurpleConnection* gc, const ReceivedCb& received_cb)
        : m_gc(gc),
          m_received_cb(received_cb)
    {
    }

    ~MessageReceiver()
    {
    }

    // Runs messages.get from given offset.
    void run_unread(int offset);
    // Processes result of messages.get and messages.getById
    int process_result(const picojson::value& result);
    // Processes attachments: appends urls to message text, adds thumbnail_urls.
    void process_attachments(const picojson::array& items, ReceivedMessage& message) const;
    // Downloads the given thumbnail for given message, modifies corresponding message text
    // and calls either next download_thumbnail() or finish(). message is index into m_messages,
    // thumbnail is index into thumbnail_urls.
    void download_thumbnail(size_t message, size_t thumbnail);
    // Sorts received messages, sends them to libpurple client and destroys this.
    void finish();
};

} // End of anonymous namespace

void receive_unread_messages(PurpleConnection* gc, const ReceivedCb& received_cb)
{
    MessageReceiver* receiver = MessageReceiver::create(gc, received_cb);
    receiver->run_unread();
}

void receive_messages(PurpleConnection* gc, const uint64_vec& message_ids, const ReceivedCb& received_cb)
{
    MessageReceiver* receiver = MessageReceiver::create(gc, received_cb);
    receiver->run(message_ids);
}

namespace
{

MessageReceiver* MessageReceiver::create(PurpleConnection* gc, const ReceivedCb& recevied_cb)
{
    return new MessageReceiver(gc, recevied_cb);
}

void MessageReceiver::run_unread()
{
    run_unread(0);
}

void MessageReceiver::run(const uint64_vec& message_ids)
{
    string ids_str = str_concat_int(',', message_ids);
    CallParams params = { {"message_ids", ids_str} };
    vk_call_api(m_gc, "messages.getById", params, [=](const picojson::value& result) {
        process_result(result);
        download_thumbnail(0, 0);
    }, [=](const picojson::value&) {
        finish();
    });
}

void MessageReceiver::run_unread(int offset)
{
    CallParams params = { {"out", "0"}, {"filters", "1"}, {"offset", str_format("%d", offset)} };
    vk_call_api(m_gc, "messages.get", params, [=](const picojson::value& result) {
        int item_count = process_result(result);
        if (item_count == 0) {
            // We ignore "count" parameter in result and increase offset until it returns empty list.
            download_thumbnail(0, 0);
            return;
        }
        run_unread(offset + item_count);
    }, [=](const picojson::value&) {
        finish();
    });
}

int MessageReceiver::process_result(const picojson::value& result)
{
    if (!field_is_present<double>(result, "count") || !field_is_present<picojson::array>(result, "items")) {
        purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                           result.serialize().data());
        return 0;
    }

    const picojson::array& items = result.get("items").get<picojson::array>();
    for (const picojson::value& v: items) {
        if (!field_is_present<double>(v, "user_id") || !field_is_present<double>(v, "date")
                || !field_is_present<string>(v, "body") || !field_is_present<double>(v, "id")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               result.serialize().data());
            continue;
        }

        uint64 uid = v.get("user_id").get<double>();
        uint64 mid = v.get("id").get<double>();
        uint64 timestamp = v.get("date").get<double>();

        // NOTE:
        //  * We must escape text, otherwise we cannot receive comment, containing &amp; or <br> as libpurple
        //    will wrongfully interpret them as markup.
        //  * Links are returned as plaintext, linkified by Pidgin etc.
        //  * Smileys are returned as Unicode emoji (both emoji and pseudocode smileys are accepted on message send).
        char* escaped = purple_markup_escape_text(v.get("body").get<string>().data(), -1);
        string text = escaped;
        g_free(escaped);

        m_messages.push_back({ uid, mid, text, timestamp, {} });

        // Process attachments: append information to text.
        if (field_is_present<picojson::array>(v, "attachments"))
            process_attachments(v.get("attachments").get<picojson::array>(), m_messages.back());
    }
    return items.size();
}

void MessageReceiver::process_attachments(const picojson::array& items, ReceivedMessage& message) const
{
    for (const picojson::value& v: items) {
        if (!field_is_present<string>(v, "type")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().data());
            return;
        }
        const string& type = v.get("type").get<string>();
        if (!field_is_present<picojson::object>(v, type)) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().data());
            return;
        }
        const picojson::value& fields = v.get(type);

        if (!message.text.empty())
            message.text += "<br>";

        if (type == "photo") {
            if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
                    || !field_is_present<string>(fields, "text") || !field_is_present<string>(fields, "photo_604")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().data());
                continue;
            }
            const uint64 id = fields.get("id").get<double>();
            const int64 owner_id = fields.get("owner_id").get<double>();
            const string& photo_text = fields.get("text").get<string>();
            const string& thumbnail = fields.get("photo_604").get<string>();

            // Apparently, there is no URL for private photos (such as the one for docs:
            // http://vk.com/docXXX_XXX?hash="access_key". If we've got "access_key" as a parameter, it means
            // that the photo is private, so we should rather link to the biggest version of the photo.
            string url;
            if (field_is_present<string>(fields, "access_key")) {
                // We have to find the max photo URL, as we do not always receive all sizes.
                if (field_is_present<string>(fields, "photo_2560"))
                    url = fields.get("photo_2560").get<string>();
                else if (field_is_present<string>(fields, "photo_1280"))
                    url = fields.get("photo_1280").get<string>();
                else if (field_is_present<string>(fields, "photo_807"))
                    url = fields.get("photo_807").get<string>();
                else
                    url = thumbnail;
            } else {
                url = str_format("http://vk.com/photo%lld_%llu", (long long)owner_id, (unsigned long long)id);
            }

            if (!photo_text.empty())
                message.text += str_format("<a href='%s'>%s</a>", url.data(), photo_text.data());
            else
                message.text += str_format("<a href='%s'>%s</a>", url.data(), url.data());
            // We append placeholder text, so that we can replace it later in download_thumbnail.
            message.text += str_format("<br><thumbnail-placeholder-%d>", message.thumbnail_urls.size());
            message.thumbnail_urls.push_back(thumbnail);
        } else if (type == "video") {
            if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
                    || !field_is_present<string>(fields, "title") || !field_is_present<string>(fields, "photo_320")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().data());
                continue;
            }
            const uint64 id = fields.get("id").get<double>();
            const int64 owner_id = fields.get("owner_id").get<double>();
            const string& title = fields.get("title").get<string>();
            const string& thumbnail = fields.get("photo_320").get<string>();

            message.text += str_format("<a href='http://vk.com/video%lld_%llu'>%s</a>", (long long)owner_id,
                                       (unsigned long long)id, title.data());
            // We append placeholder text, so that we can replace it later in download_thumbnail.
            message.text += str_format("<br><thumbnail-placeholder-%d>", message.thumbnail_urls.size());
            message.thumbnail_urls.push_back(thumbnail);
        } else if (type == "audio") {
            if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "artist")
                    || !field_is_present<string>(fields, "title")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().data());
                continue;
            }
            const string& url = fields.get("url").get<string>();
            const string& artist = fields.get("artist").get<string>();
            const string& title = fields.get("title").get<string>();

            message.text += str_format("<a href='%s'>%s - %s</a>", url.data(), artist.data(), title.data());
        } else if (type == "doc") {
            if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "title")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().data());
                continue;
            }
            const string& url = fields.get("url").get<string>();
            const string& title = fields.get("title").get<string>();

            message.text += str_format("<a href='%s'>%s</a>", url.data(), title.data());
        } else {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().data());
            message.text += "\nUnknown attachement type ";
            message.text += type;
            continue;
        }
    }
}

void MessageReceiver::download_thumbnail(size_t message, size_t thumbnail)
{
    if (message >= m_messages.size()) {
        finish();
        return;
    }
    if (thumbnail >= m_messages[message].thumbnail_urls.size()) {
        download_thumbnail(message + 1, 0);
        return;
    }

    const string& url = m_messages[message].thumbnail_urls[thumbnail];
    http_get(m_gc, url, [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        if (!purple_http_response_is_successful(response)) {
            purple_debug_error("prpl-vkcom", "Unable to download thumbnail: %s\n",
                               purple_http_response_get_error(response));
            download_thumbnail(message, thumbnail + 1);
            return;
        }

        size_t size;
        const char* data = purple_http_response_get_data(response, &size);
        int img_id = purple_imgstore_add_with_id(g_memdup(data, size), size, nullptr);

        string img_tag = str_format("<img id=\"%d\">", img_id);
        string img_placeholder = str_format("<thumbnail-placeholder-%d>", thumbnail);
        str_replace(m_messages[message].text, img_placeholder, img_tag);

        download_thumbnail(message, thumbnail + 1);
    });
}

void MessageReceiver::finish()
{
    std::sort(m_messages.begin(), m_messages.end(), [](const ReceivedMessage& a, const ReceivedMessage& b) {
        return a.timestamp < b.timestamp;
    });

    uint64_vec message_ids;
    for (const ReceivedMessage& m: m_messages) {
        serv_got_im(m_gc, buddy_name_from_uid(m.uid).data(), m.text.data(), PURPLE_MESSAGE_RECV, m.timestamp);
        message_ids.push_back(m.mid);
    }
    mark_message_as_read(m_gc, message_ids);

    if (m_received_cb)
        m_received_cb();
    delete this;
}

} // End of anonymous namespace

void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids)
{
    if (message_ids.empty())
        return;
    // Creates string of identifiers, separated with comma.
    string ids_str = str_concat_int(',', message_ids);

    CallParams params = { {"message_ids", ids_str} };
    vk_call_api(gc, "messages.markAsRead", params);
}