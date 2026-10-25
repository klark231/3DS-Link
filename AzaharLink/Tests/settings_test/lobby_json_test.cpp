// Built and run by Tests/run.sh (g++ -std=gnu++17 -fno-exceptions -fno-rtti -I ConfigApp/source Tests/settings_test/lobby_json_test.cpp; an optional argument is a reply saved with curl).
#include <cstdio>
#include <fstream>
#include <sstream>

#include "lobby_json.hpp"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    // The shape of a real reply (the first rooms of http://88.198.47.46:5000/lobby): a big gameId, a \u escape, a password room, an empty players list.
    const char* sample =
        "{\"rooms\":[{\"active\":true,\"address\":\"184.56.91.184\",\"description\":\"Citra-Based Room for Pok\\u00e9mon Titles\","
        "\"externalGuid\":\"184.56.91.184-24873\",\"hasPassword\":false,\"id\":\"x\",\"maxPlayers\":100,\"name\":\"   All Purpose Pokemon Room\","
        "\"netVersion\":4,\"owner\":\"\",\"players\":[{\"gameId\":1125899907194368,\"gameName\":\"Pok\\u00e9mon Y\",\"nickname\":\"VOSH\"},"
        "{\"gameId\":0,\"gameName\":\"\",\"nickname\":\"Vedy\"}],\"port\":24873,\"preferredGameId\":1125899908632832,"
        "\"preferredGameName\":\"Trade & Battle\",\"timestamp\":1793225841},"
        "{\"active\":true,\"address\":\"129.146.135.99\",\"hasPassword\":true,\"maxPlayers\":6,\"name\":\"(ANTHENA #1 US) MH4G,MH4U\","
        "\"netVersion\":4,\"players\":[],\"port\":10001,\"preferredGameName\":\"Monster Hunter\"},"
        "{\"active\":false,\"address\":\"1.2.3.4\",\"port\":5,\"name\":\"gone\"},"
        "{\"address\":\"\",\"port\":5,\"name\":\"no address\"}],\"extra\":{\"a\":[1,2,{\"b\":null}]}}";
    std::vector<lobby::Room> rooms;
    std::string err;
    CHECK(lobby::Parse(sample, rooms, &err));
    CHECK(rooms.size() == 2);
    if (rooms.size() == 2) {
        CHECK(rooms[0].address == "184.56.91.184" && rooms[0].port == 24873 && rooms[0].max_players == 100);
        CHECK(rooms[0].players == 2 && rooms[0].nicknames.size() == 2 && rooms[0].nicknames[0] == "VOSH");
        CHECK(rooms[0].description == "Citra-Based Room for Pok\xC3\xA9mon Titles");
        CHECK(rooms[1].has_password && rooms[1].players == 0 && rooms[1].preferred_game == "Monster Hunter" && rooms[1].port == 10001);
    }
    CHECK(lobby::Parse("{\"rooms\":[]}", rooms) && rooms.empty());
    CHECK(!lobby::Parse("<!doctype html><title>404</title>", rooms, &err));
    CHECK(!lobby::Parse("{\"rooms\":[{\"name\":\"cut", rooms, &err));
    CHECK(!lobby::Parse("{\"other\":1}", rooms, &err));
    CHECK(!lobby::Parse("", rooms));
    std::string deep = "{\"x\":";
    for (int i = 0; i < 5000; ++i) deep += "[";
    CHECK(!lobby::Parse(deep, rooms));   // no stack overflow
    if (argc > 1) { // a real reply saved with curl
        std::ifstream f(argv[1]);
        std::stringstream ss;
        ss << f.rdbuf();
        const bool ok = lobby::Parse(ss.str(), rooms, &err);
        std::printf("%s: %s, %zu rooms\n", argv[1], ok ? "parsed" : err.c_str(), rooms.size());
        for (size_t i = 0; i < rooms.size() && i < 8; ++i)
            std::printf("  %-28.28s %s:%u  %u/%u%s  [%s]\n", rooms[i].name.c_str(), rooms[i].address.c_str(), rooms[i].port, rooms[i].players,
                        rooms[i].max_players, rooms[i].has_password ? " pw" : "", rooms[i].preferred_game.c_str());
    }
    std::printf(fails ? "lobby_json_test: %d FAILED\n" : "lobby_json_test: ALL OK\n", fails);
    return fails ? 1 : 0;
}
