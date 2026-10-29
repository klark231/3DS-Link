// Built and run by Tests/run.sh: the chat core (Includes/azl/chat.hpp) and its packets (room_proto).
#include <cstdio>

#include "azl/chat.hpp"
#include "azl/room_proto.hpp"

using namespace azl;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static const chat::Key* KeyFor(char c, bool shifted = false) {
    std::size_t n = 0;
    const chat::Key* k = chat::Keys(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (k[i].kind == chat::Key::Char && (shifted ? k[i].shifted : k[i].normal) == c) return &k[i];
    }
    return nullptr;
}
static const chat::Key* KindKey(chat::Key::Kind kind) {
    std::size_t n = 0;
    const chat::Key* k = chat::Keys(&n);
    for (std::size_t i = 0; i < n; ++i) if (k[i].kind == kind) return &k[i];
    return nullptr;
}
static std::uint64_t t = 1000;
static chat::Keyboard::Event Tap(chat::Keyboard& kb, const chat::Key* k) {
    const int x = k->x + k->w / 2, y = k->y + k->h / 2;
    t += 50;
    auto e = kb.OnTouch(true, x, y, t);
    t += 50;
    kb.OnTouch(false, 0, 0, t);
    return e;
}

int main() {
    // --- cleaning: ASCII only, no control characters, collapsed spaces, cut
    CHECK(chat::Clean("  hello   world \n\t!", 100) == "hello world !");
    CHECK(chat::Clean("caf\xC3\xA9 ok", 100) == "caf ok");              // the two bytes of the e-acute are dropped
    CHECK(chat::Clean("\x1b[31mred\x07", 100) == "[31mred");             // ESC and BEL are gone
    CHECK(chat::Clean("abcdefghij", 4) == "abcd");
    CHECK(chat::Clean("", 10).empty());

    // --- packets
    std::vector<u8> pkt = room::EncodeChat("hi there");
    CHECK(pkt.size() == 1 + 4 + 8 && pkt[0] == room::IdChatMessage);
    std::string big(900, 'x');
    CHECK(room::EncodeChat(big).size() == 1 + 4 + room::kMaxChatChars);
    // a relayed packet: [id][nickname][username][message]
    room::PacketWriter w;
    w.U8(room::IdChatMessage); w.String("Vedy"); w.String("account"); w.String("ready?");
    std::string nick, msg;
    CHECK(room::DecodeChat(w.data().data(), w.data().size(), &nick, &msg) && nick == "Vedy" && msg == "ready?");
    CHECK(!room::DecodeChat(w.data().data(), 7, &nick, &msg));          // cut short
    u8 wrong[8] = {room::IdWifiPacket, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!room::DecodeChat(wrong, sizeof(wrong), &nick, &msg));

    // --- the log: oldest first, only what is recent, the ring forgets the oldest
    chat::Log log;
    chat::Log::Line out[chat::kLines];
    CHECK(log.Visible(10000, out) == 0);
    log.Push("A", "one", 10000);
    log.Push("B", "two", 11000);
    CHECK(log.Visible(12000, out) == 2 && out[0].text == "A: one" && out[1].text == "B: two");
    CHECK(log.Visible(10000 + chat::kShowMs + 1, out) == 1 && out[0].text == "B: two");   // the first expired
    for (int i = 0; i < 6; ++i) log.Push("C", "line" + std::to_string(i), 20000 + i);
    CHECK(log.Visible(20010, out) == chat::kLines && out[0].text == "C: line2" && out[3].text == "C: line5");
    log.Push("", std::string(300, 'z'), 30000);
    CHECK(log.Visible(30001, out) >= 1 && out[chat::kLines - 1].text.size() <= chat::kLineChars);
    log.Push("D", "\x01\x02", 31000);                                    // nothing printable: not shown
    CHECK(log.Visible(31001, out) >= 1 && out[0].text.find("D:") == std::string::npos);

    // --- the keyboard
    chat::Keyboard kb;
    CHECK(!kb.is_open());
    CHECK(kb.OnTouch(true, 5, 200, 1000) == chat::Keyboard::Event::None);   // a tap elsewhere does nothing while closed
    kb.OnTouch(false, 0, 0, 1050);
    CHECK(kb.OnTouch(true, chat::kChatButton.x + 5, chat::kChatButton.y + 5, 1100) == chat::Keyboard::Event::Opened && kb.is_open());
    kb.OnTouch(false, 0, 0, 1150);
    CHECK(kb.OnTouch(true, 100, 5, 1200) == chat::Keyboard::Event::None && kb.is_open());   // a tap outside the keys is ignored
    kb.OnTouch(false, 0, 0, 1250);
    Tap(kb, KeyFor('h')); Tap(kb, KeyFor('i'));
    CHECK(kb.text() == "hi");
    Tap(kb, KindKey(chat::Key::Shift)); Tap(kb, KeyFor('y'));
    CHECK(kb.text() == "hiY" && !kb.shift());                         // shift lasts for one letter
    Tap(kb, KindKey(chat::Key::Shift)); Tap(kb, KeyFor('!', true));    // the '1' key with shift: '!'
    CHECK(kb.text() == "hiY!" && !kb.shift());
    Tap(kb, KindKey(chat::Key::Back));
    CHECK(kb.text() == "hiY");
    Tap(kb, KindKey(chat::Key::Space));
    CHECK(kb.text() == "hiY ");
    CHECK(Tap(kb, KindKey(chat::Key::Send)) == chat::Keyboard::Event::Send && kb.TakeMessage() == "hiY" && kb.text().empty());
    CHECK(kb.is_open());                                                    // stays open for the next line
    CHECK(Tap(kb, KindKey(chat::Key::Send)) == chat::Keyboard::Event::None);   // an empty box sends nothing
    // held touch must not repeat characters; backspace repeats after 450 ms
    Tap(kb, KeyFor('a')); Tap(kb, KeyFor('b')); Tap(kb, KeyFor('c'));
    const chat::Key* back = KindKey(chat::Key::Back);
    const int bx = back->x + 4, by = back->y + 4;
    kb.OnTouch(true, bx, by, 5000);                                      // press: erases one
    CHECK(kb.text() == "ab");
    kb.OnTouch(true, bx, by, 5200);
    CHECK(kb.text() == "ab");                                            // still inside the 450 ms
    kb.OnTouch(true, bx, by, 5500);
    CHECK(kb.text() == "a");                                             // repeat
    kb.OnTouch(false, 0, 0, 5600);
    // the length limit
    for (int i = 0; i < 150; ++i) Tap(kb, KeyFor('x'));
    CHECK(kb.text().size() <= chat::kTypedChars);
    CHECK(kb.Tail(10).size() == 10);
    CHECK(Tap(kb, KindKey(chat::Key::Close)) == chat::Keyboard::Event::Closed && !kb.is_open());
    // every key lies inside the 320 x 240 bottom screen and the keys do not overlap
    std::size_t n = 0;
    const chat::Key* keys = chat::Keys(&n);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(keys[i].x >= 0 && keys[i].x + keys[i].w <= 320 && keys[i].y >= chat::kKeysY && keys[i].y + keys[i].h <= 240);
        for (std::size_t j = i + 1; j < n; ++j) {
            const bool apart = keys[i].x + keys[i].w <= keys[j].x || keys[j].x + keys[j].w <= keys[i].x ||
                               keys[i].y + keys[i].h <= keys[j].y || keys[j].y + keys[j].h <= keys[i].y;
            CHECK(apart);
        }
    }
    std::printf(fails ? "chat_test: %d FAILED\n" : "chat_test: ALL OK\n", fails);
    return fails ? 1 : 0;
}
