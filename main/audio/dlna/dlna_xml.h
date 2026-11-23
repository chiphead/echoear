#pragma once

#include <string>
#include <utility>

namespace DlnaXml {

inline std::string Escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

inline std::string BuildDidlMetadata(const std::string& media_uri,
                                       const std::string& title) {
    const std::string safe_title = title.empty() ? "EchoEar audio" : title;
    return
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"0\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>" + Escape(safe_title) + "</dc:title>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/wav:*\">" +
        Escape(media_uri) + "</res></item></DIDL-Lite>";
}

inline std::string Unescape(std::string value) {
    const std::pair<const char*, const char*> entities[] = {
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"},
        {"&gt;", ">"}, {"&amp;", "&"},
    };
    for (const auto& entity : entities) {
        size_t pos = 0;
        while ((pos = value.find(entity.first, pos)) != std::string::npos) {
            value.replace(pos, std::char_traits<char>::length(entity.first),
                          entity.second);
            pos += std::char_traits<char>::length(entity.second);
        }
    }
    return value;
}

inline bool ExtractValue(const std::string& xml, const char* tag,
                         std::string& value) {
    if (tag == nullptr || *tag == '\0') return false;
    const std::string wanted(tag);
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t name_begin = pos + 1;
        if (name_begin >= xml.size() || xml[name_begin] == '/' ||
            xml[name_begin] == '!' || xml[name_begin] == '?') {
            ++pos;
            continue;
        }
        const size_t name_end = xml.find_first_of(" />\t\r\n", name_begin);
        if (name_end == std::string::npos) return false;
        const std::string qualified = xml.substr(name_begin, name_end - name_begin);
        const size_t colon = qualified.rfind(':');
        const std::string local = colon == std::string::npos
            ? qualified : qualified.substr(colon + 1);
        if (local != wanted) {
            pos = name_end;
            continue;
        }
        const size_t open_end = xml.find('>', name_end);
        if (open_end == std::string::npos) return false;
        const std::string close = "</" + qualified + ">";
        const size_t close_pos = xml.find(close, open_end + 1);
        if (close_pos == std::string::npos) return false;
        value = Unescape(xml.substr(open_end + 1, close_pos - open_end - 1));
        return !value.empty();
    }
    return false;
}

}  // namespace DlnaXml
