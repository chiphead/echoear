#include "dlna_xml.h"

#include <cassert>
#include <string>

int main() {
    assert(DlnaXml::Escape("a&b<c>d\"e'f") ==
           "a&amp;b&lt;c&gt;d&quot;e&apos;f");

    const std::string metadata =
        DlnaXml::BuildDidlMetadata("http://192.0.2.1/a?x=1&y=2", "T&<est");
    assert(metadata.find("xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"") !=
    assert(metadata.find('\\\\') == std::string::npos);
    assert(metadata.find("xmlns=\\\\\"") == std::string::npos);
    assert(metadata.find("<dc:title>T&amp;&lt;est</dc:title>") != std::string::npos);
    assert(metadata.find("http://192.0.2.1/a?x=1&amp;y=2") != std::string::npos);
    assert(DlnaXml::BuildDidlMetadata("http://example.test/a.wav", "").find(
               "<dc:title>EchoEar audio</dc:title>") != std::string::npos);

    std::string value;
    const std::string namespaced =
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"urn:soap\">"
        "<s:Body><u:GetMediaInfoResponse xmlns:u=\"urn:av\">"
        "<u:CurrentURI>http://192.0.2.1/audio?a=1&amp;b=two%20words</u:CurrentURI>"
        "</u:GetMediaInfoResponse></s:Body></s:Envelope>";
    assert(DlnaXml::ExtractValue(namespaced, "CurrentURI", value));
    assert(value == "http://192.0.2.1/audio?a=1&b=two%20words");

    const std::string escaped =
        "<x:Response><x:CurrentURI>urn:test:&quot;quoted&quot;&apos;</x:CurrentURI>"
        "</x:Response>";
    assert(DlnaXml::ExtractValue(escaped, "CurrentURI", value));
    assert(value == "urn:test:\"quoted\"'");

    const std::string plain =
        "<GetTransportInfoResponse><CurrentTransportState>PLAYING</CurrentTransportState>"
        "</GetTransportInfoResponse>";
    assert(DlnaXml::ExtractValue(plain, "CurrentTransportState", value));
    assert(value == "PLAYING");

    assert(!DlnaXml::ExtractValue(plain, "CurrentURI", value));
    assert(!DlnaXml::ExtractValue("<x:CurrentURI>broken", "CurrentURI", value));
    assert(!DlnaXml::ExtractValue("<x:CurrentURI></x:CurrentURI>", "CurrentURI", value));
    assert(!DlnaXml::ExtractValue(plain, nullptr, value));
    assert(!DlnaXml::ExtractValue(plain, "", value));

    // Decode entities once: an encoded literal entity must remain encoded.
    assert(DlnaXml::Unescape("&amp;lt;") == "&lt;");
    return 0;
}
