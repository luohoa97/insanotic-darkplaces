/*
 * Insanotic: data for RmlUi documents that show lists.
 *
 * The server browser and the campaign list are made of rows the engine
 * knows (the server list that the network code fills, the campaign file of
 * the game). RmlUi's data models bind them to a document without scripting:
 * a document says data-model="servers" and repeats a row with data-for, and
 * the functions here fill the rows and answer the data-event-click calls.
 * See data/insanotic.pk3dir/ui/servers.rml and singleplayer.rml.
 *
 * Only documents of the game may use these models. A page from a server is
 * refused when it names any (UIRml_ShowServer in ui_rml.cpp).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <RmlUi/Core.h>

#include <algorithm>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ui_rml.h"

namespace {

// Xonotic names carry color codes: ^1 and ^x123, and ^^ for a caret
std::string StripColors(const char* s)
{
	std::string out;
	for (; *s; ++s) {
		if (*s != '^') {
			out += *s;
			continue;
		}
		if (s[1] == '^') {
			out += '^';
			++s;
		} else if (s[1] >= '0' && s[1] <= '9') {
			++s;
		} else if ((s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3]) && isxdigit((unsigned char)s[4])) {
			s += 4;
		} else {
			out += '^';
		}
	}
	return out;
}

// text that goes into an RML string (data-rml): escaped, with line breaks
std::string ToRml(const std::string& in)
{
	std::string out;
	for (char c : in) {
		switch (c) {
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '&': out += "&amp;"; break;
		case '\n': out += "<br/>"; break;
		default: out += c; break;
		}
	}
	return out;
}

std::string Upper(std::string s)
{
	for (char& c : s)
		c = (char)toupper((unsigned char)c);
	return s;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

// ---------------------------------------------------------------------------
// Servers
// ---------------------------------------------------------------------------

struct PlayerRow {
	std::string name;
	int score = 0, ping = 0;
	bool spectator = false;
};

// net_slist_favorites: addresses (or key hashes) separated by spaces
std::vector<std::string> Favorites()
{
	std::vector<std::string> out;
	std::string s = UIE_CvarString("net_slist_favorites"), cur;
	for (char c : s) {
		if (c == ' ' || c == '"') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else
			cur += c;
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

bool IsFavorite(const std::string& address)
{
	for (const std::string& f : Favorites())
		if (f == address)
			return true;
	return false;
}

// adds the address, or takes it out when it is there
void ToggleFavorite(const std::string& address)
{
	std::vector<std::string> list = Favorites();
	std::string out;
	bool found = false;
	for (const std::string& f : list) {
		if (f == address) {
			found = true;
			continue;
		}
		out += (out.empty() ? "" : " ") + f;
	}
	if (!found)
		out += (out.empty() ? "" : " ") + address;
	UIE_CvarSet("net_slist_favorites", out.c_str());
}

// the player lines of a server: score ping team "name"; team 0 is a spectator
std::vector<PlayerRow> ParsePlayers(const char* text)
{
	std::vector<PlayerRow> out;
	std::string all = text;
	size_t start = 0;
	while (start < all.size()) {
		size_t end = all.find('\n', start);
		if (end == std::string::npos)
			end = all.size();
		std::string line = all.substr(start, end - start);
		start = end + 1;
		std::vector<std::string> f;
		std::string cur;
		bool quoted = false;
		for (char c : line) {
			if (c == '"')
				quoted = !quoted;
			else if (c == ' ' && !quoted) {
				if (!cur.empty())
					f.push_back(cur);
				cur.clear();
			} else
				cur += c;
		}
		if (!cur.empty() || quoted)
			f.push_back(cur);
		if (f.size() < 3)
			continue;
		PlayerRow p;
		p.score = atoi(f[0].c_str());
		p.ping = atoi(f[1].c_str());
		p.spectator = f.size() >= 4 && f[2] == "0";
		p.name = StripColors(f.back().c_str());
		out.push_back(p);
	}
	return out;
}

struct ServerRow {
	std::string address, name, map, type, players;
	bool preview = false; // the server can send a picture of its map
	std::string image; // the map's picture, only while the row is near the screen (else empty)
	int ping = 0;
	int humans = 0;
	int slots = 0;
	int free_slots = 0;
	bool favorite = false;
};

struct ServersModel {
	// the info panel of the tapped server
	bool info_open = false;
	std::string info_name, info_address, info_map, info_image, info_mode, info_mod, info_extra;
	int info_ping = 0, info_humans = 0, info_bots = 0, info_slots = 0;
	bool info_favorite = false;
	std::vector<PlayerRow> info_players;
	Rml::DataModelHandle handle;
	std::vector<ServerRow> rows;
	std::vector<ServerRow> all; // what the network code found, before the filter
	std::string filter, selected, address, status;
	std::string sort = "ping";
	bool descending = false;
	bool show_empty = true, show_full = true, show_laggy = true;
	bool refreshing = false;
	double refresh_until = 0;
	bool was_shown = false;
	std::string signature; // what the rows were built from, to tell when they changed
	double next_update = 0;
	double next_images = 0;
	Rml::Context* context = nullptr;
	std::map<std::string, std::string> pictures; // map name -> picture path, "" when the client has none
	std::map<std::string, double> next_recheck;
	std::set<std::string> asked; // maps a server was asked for a picture of
};

ServersModel* servers = nullptr;

// The picture of a map as the menu can load it ("/maps/<name>"), or "" when
// the game files have none. Maps the player once downloaded count: their
// packs are in the file system. The answer is remembered for each map, as
// many servers share maps, and forgotten when the list is asked again, in
// case a map was downloaded in between.
std::string MapPicture(const std::string& map)
{
	if (map.empty() || map.find_first_of("/\\.:") != std::string::npos)
		return "";
	double now = UIE_Time();
	auto it = servers->pictures.find(map);
	// a map that was asked for may have arrived: look again now and then
	if (it != servers->pictures.end() && (!it->second.empty() || !servers->asked.count(map) || now < servers->next_recheck[map]))
		return it->second;
	std::string found;
	for (const char* ext : {".jpg", ".png", ".tga"})
		if (UIE_FileExists(("maps/" + map + ext).c_str())) {
			found = "/maps/" + map;
			break;
		}
	if (found.empty()) {
		// a picture a server sent earlier (netconn.c): mappreviews/<map>-<crc>.jpg
		char files[4][128];
		if (UIE_FileList(("mappreviews/" + map + "-*.jpg").c_str(), files, 4) > 0)
			found = std::string("/") + files[0];
	}
	servers->pictures[map] = found;
	servers->next_recheck[map] = now + 1.5;
	return found;
}

// Only the rows in view (and a few around them) get their picture: a
// texture is loaded for each one, and a phone should not hold hundreds.
// Rows that scroll out lose theirs again.
void UpdateRowImages()
{
	ServersModel& m = *servers;
	if (!m.context || m.rows.empty())
		return;
	Rml::Element* list = nullptr;
	for (int i = 0; i < m.context->GetNumDocuments() && !list; i++)
		list = m.context->GetDocument(i)->GetElementById("serverlist");
	if (!list || list->GetNumChildren() < 1)
		return;
	float rh = list->GetChild(0)->GetOffsetHeight();
	if (rh < 1)
		return;
	int first = std::max(0, (int)(list->GetScrollTop() / rh) - 2);
	int last = (int)((list->GetScrollTop() + list->GetClientHeight()) / rh) + 2;
	bool changed = false;
	for (int i = 0; i < (int)m.rows.size(); i++) {
		bool visible = i >= first && i <= last;
		std::string want = visible ? MapPicture(m.rows[i].map) : "";
		// a map the client has no picture of: ask a server that has one (a few at
		// a time, once per map; the answer is a file that the lookup finds)
		if (visible && want.empty() && m.rows[i].preview && !m.asked.count(m.rows[i].map) && UIE_MapPreviewRequest(m.rows[i].address.c_str(), m.rows[i].map.c_str()) != 0)
			m.asked.insert(m.rows[i].map);
		if (m.rows[i].image != want) {
			m.rows[i].image = want;
			changed = true;
		}
	}
	if (changed)
		m.handle.DirtyVariable("rows");
}

bool CvarBool(const char* name, bool fallback)
{
	const char* s = UIE_CvarString(name);
	return *s ? atoi(s) != 0 : fallback;
}

void ServersRebuild(bool force)
{
	ServersModel& m = *servers;
	std::vector<UIE_Server> raw(1024);
	int n = UIE_ServerList(raw.data(), (int)raw.size());
	int max_ping = atoi(UIE_CvarString("menu_slist_maxping"));
	if (max_ping <= 0)
		max_ping = 200;

	std::string filter = Lower(m.filter);
	std::vector<ServerRow> rows;
	for (int i = 0; i < n; i++) {
		const UIE_Server& s = raw[i];
		ServerRow r;
		r.address = s.cname;
		r.name = StripColors(s.name);
		r.map = s.map;
		r.type = Upper(s.type);
		r.ping = s.ping;
		r.humans = s.numhumans;
		r.slots = s.maxplayers;
		r.free_slots = s.freeslots;
		r.favorite = IsFavorite(r.address);
		r.preview = s.mappreview != 0;
		r.players = std::to_string(s.numhumans) + "/" + std::to_string(s.maxplayers);
		if (!m.show_empty && s.numhumans < 1)
			continue;
		if (!m.show_full && s.freeslots < 1)
			continue;
		if (!m.show_laggy && s.ping > max_ping)
			continue;
		if (!filter.empty() && Lower(r.name).find(filter) == std::string::npos && Lower(r.map).find(filter) == std::string::npos &&
		    Lower(r.type).find(filter) == std::string::npos)
			continue;
		rows.push_back(r);
	}
	bool desc = m.descending;
	// favorites stay on top, whatever the order
	const std::string key = m.sort;
	std::stable_sort(rows.begin(), rows.end(), [&](const ServerRow& a, const ServerRow& b) {
		int c;
		if (a.favorite != b.favorite)
			return a.favorite;
		if (key == "name")
			c = Lower(a.name).compare(Lower(b.name));
		else if (key == "map")
			c = a.map.compare(b.map);
		else if (key == "type")
			c = a.type.compare(b.type);
		else if (key == "players")
			c = a.humans - b.humans;
		else
			c = a.ping - b.ping;
		return desc ? c > 0 : c < 0;
	});

	std::string sig = std::to_string(n) + ":" + std::to_string(rows.size());
	for (const ServerRow& r : rows)
		sig += "|" + r.address + "," + r.name + "," + std::to_string(r.ping) + "," + r.players;
	std::string status = std::to_string(rows.size()) + (rows.size() == 1 ? " server" : " servers");
	if (n != (int)rows.size())
		status += " (" + std::to_string(n) + " found)";
	if (m.refreshing)
		status += ", refreshing...";
	if (!force && sig == m.signature && status == m.status)
		return;
	m.signature = sig;
	m.rows.swap(rows);
	UpdateRowImages(); // so the pictures do not blink when the list changes
	m.status = status;
	m.handle.DirtyVariable("rows");
	m.handle.DirtyVariable("status");
}

void InfoRefresh()
{
	ServersModel& m = *servers;
	if (!m.info_open)
		return;
	UIE_ServerDetail d;
	if (!UIE_ServerDetail_Get(m.info_address.c_str(), &d))
		return;
	m.info_players = ParsePlayers(d.players);
	m.info_humans = d.numhumans;
	m.info_bots = d.numbots;
	m.info_slots = d.maxplayers;
	m.info_mod = d.mod;
	m.info_favorite = IsFavorite(m.info_address);
	for (const ServerRow& r : m.rows)
		if (r.address == m.info_address) {
			m.info_ping = r.ping;
			m.info_name = r.name;
			m.info_map = r.map;
			m.info_image = MapPicture(r.map);
			m.info_mode = r.type;
		}
	for (const char* v : {"info_players", "info_humans", "info_bots", "info_slots", "info_mod", "info_favorite", "info_ping", "info_name", "info_map", "info_image", "info_mode"})
		m.handle.DirtyVariable(v);
}

void ServersCvars(bool write)
{
	ServersModel& m = *servers;
	struct { const char* cvar; bool* value; } map[] = {
		{"menu_slist_showempty", &m.show_empty}, {"menu_slist_showfull", &m.show_full}, {"menu_slist_showlaggy", &m.show_laggy}};
	static bool last[3] = {true, true, true};
	for (int i = 0; i < 3; i++) {
		if (write) {
			if (*map[i].value != last[i]) {
				UIE_CvarSet(map[i].cvar, *map[i].value ? "1" : "0");
				last[i] = *map[i].value;
			}
		} else {
			last[i] = CvarBool(map[i].cvar, true);
			*map[i].value = last[i];
		}
	}
}

void ServersAsk(double now, bool reset)
{
	UIE_ServerRefresh(reset ? 1 : 0);
	servers->pictures.clear();
	servers->next_recheck.clear();
	servers->refreshing = true;
	servers->refresh_until = now + 6; // long enough for the first answers
	servers->next_update = 0;
	servers->signature.clear();
}

void ServersUpdate(double now, bool shown)
{
	if (!servers)
		return;
	// opening the page asks the master servers, like the game's own list does
	if (shown && !servers->was_shown)
		ServersAsk(now, false);
	servers->was_shown = shown;
	if (servers->refreshing && now > servers->refresh_until) {
		servers->refreshing = false;
		servers->signature.clear();
	}
	if (!shown)
		return;
	if (now >= servers->next_images) {
		servers->next_images = now + 0.12;
		UpdateRowImages();
	}
	if (now < servers->next_update)
		return;
	servers->next_update = now + 0.4;
	ServersCvars(true);
	ServersRebuild(false);
	InfoRefresh();
}

void CreateServers(Rml::Context* context)
{
	Rml::DataModelConstructor c = context->CreateDataModel("servers");
	if (!c)
		return;
	servers = new ServersModel;
	servers->context = context;
	ServersModel& m = *servers;
	if (auto s = c.RegisterStruct<ServerRow>()) {
		s.RegisterMember("address", &ServerRow::address);
		s.RegisterMember("image", &ServerRow::image);
		s.RegisterMember("name", &ServerRow::name);
		s.RegisterMember("map", &ServerRow::map);
		s.RegisterMember("type", &ServerRow::type);
		s.RegisterMember("players", &ServerRow::players);
		s.RegisterMember("ping", &ServerRow::ping);
		s.RegisterMember("favorite", &ServerRow::favorite);
	}
	if (auto p = c.RegisterStruct<PlayerRow>()) {
		p.RegisterMember("name", &PlayerRow::name);
		p.RegisterMember("score", &PlayerRow::score);
		p.RegisterMember("ping", &PlayerRow::ping);
		p.RegisterMember("spectator", &PlayerRow::spectator);
	}
	c.RegisterArray<std::vector<PlayerRow>>();
	c.RegisterArray<std::vector<ServerRow>>();
	c.Bind("info_open", &m.info_open);
	c.Bind("info_name", &m.info_name);
	c.Bind("info_address", &m.info_address);
	c.Bind("info_map", &m.info_map);
	c.Bind("info_image", &m.info_image);
	c.Bind("info_mode", &m.info_mode);
	c.Bind("info_mod", &m.info_mod);
	c.Bind("info_ping", &m.info_ping);
	c.Bind("info_humans", &m.info_humans);
	c.Bind("info_bots", &m.info_bots);
	c.Bind("info_slots", &m.info_slots);
	c.Bind("info_favorite", &m.info_favorite);
	c.Bind("info_players", &m.info_players);
	c.Bind("rows", &m.rows);
	c.Bind("filter", &m.filter);
	c.Bind("selected", &m.selected);
	c.Bind("address", &m.address);
	c.Bind("status", &m.status);
	c.Bind("show_empty", &m.show_empty);
	c.Bind("show_full", &m.show_full);
	c.Bind("show_laggy", &m.show_laggy);
	c.Bind("sort", &m.sort);
	c.Bind("descending", &m.descending);
	m.handle = c.GetModelHandle();

	c.BindEventCallback("select", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty())
			return;
		servers->selected = args[0].Get<std::string>();
		servers->address = servers->selected;
		// a tap opens the panel with what the server told
		servers->info_open = true;
		servers->info_address = servers->selected;
		h.DirtyVariable("selected");
		h.DirtyVariable("address");
		h.DirtyVariable("info_open");
		h.DirtyVariable("info_address");
		InfoRefresh();
	});
	c.BindEventCallback("info_close", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList&) {
		servers->info_open = false;
		h.DirtyVariable("info_open");
	});
	c.BindEventCallback("info_join", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		UIE_Command(("connect " + servers->info_address).c_str());
		UIE_Command("rml_hide servers");
	});
	c.BindEventCallback("toggle_favorite", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		ToggleFavorite(servers->info_address);
		servers->signature.clear();
		ServersRebuild(true);
		InfoRefresh();
	});
	c.BindEventCallback("join", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		// an address typed by hand may hold anything, a console line must not
		std::string a;
		for (char ch : servers->address)
			if (ch > ' ' && ch != ';' && ch != '"' && ch != '\\' && ch != '$')
				a += ch;
		if (a.empty())
			return;
		UIE_Command(("connect " + a).c_str());
		UIE_Command("rml_hide servers");
	});
	c.BindEventCallback("refresh", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ServersAsk(UIE_Time(), true); });
	c.BindEventCallback("sort", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty())
			return;
		std::string key = args[0].Get<std::string>();
		if (key == servers->sort)
			servers->descending = !servers->descending;
		else {
			servers->sort = key;
			servers->descending = key == "players";
		}
		h.DirtyVariable("sort");
		h.DirtyVariable("descending");
		ServersRebuild(true);
	});
	c.BindEventCallback("filter_changed", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ServersRebuild(true); });
	c.BindEventCallback("toggled", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		ServersCvars(true);
		ServersRebuild(true);
	});
	ServersCvars(false);
}

// ---------------------------------------------------------------------------
// Campaign
// ---------------------------------------------------------------------------

struct LevelRow {
	int number = 0;
	std::string title, desc, map, mode, image, state;
	bool locked = false, current = false, done = false;
};

struct CampaignModel {
	Rml::DataModelHandle handle;
	std::vector<LevelRow> levels;
	std::string name, title;
	bool has_prev = false, has_next = false;
	int progress = 0;
	int selected = 0;
	std::string signature;
	double next_update = 0;
};

CampaignModel* campaign = nullptr;

// one line of the campaign file: comma separated, fields in quotes, "" is a quote
std::vector<std::string> SplitCsv(const std::string& line)
{
	std::vector<std::string> out;
	std::string cur;
	bool quoted = false, any = false;
	for (size_t i = 0; i < line.size(); i++) {
		char c = line[i];
		if (quoted) {
			if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
				cur += '"';
				i++;
			} else if (c == '"')
				quoted = false;
			else
				cur += c;
		} else if (c == '"') {
			quoted = true;
			any = true;
		} else if (c == ',') {
			out.push_back(cur);
			cur.clear();
			any = false;
		} else if (c != '\r')
			cur += c;
	}
	if (any || !cur.empty() || !out.empty())
		out.push_back(cur);
	return out;
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
	for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
		s.replace(p, from.size(), to);
	return s;
}

// the same columns as the game's campaign_file.qc reads
void CampaignLoad(bool force)
{
	CampaignModel& m = *campaign;
	std::string name = UIE_CvarString("g_campaign_name");
	if (name.empty())
		name = "xonoticbeta";
	int progress = atoi(UIE_CvarString(("g_campaign" + name + "_index").c_str()));

	char files[16][128];
	int nfiles = UIE_FileList("maps/campaign*.txt", files, 16);
	int pos = -1;
	for (int i = 0; i < nfiles; i++)
		if (std::string(files[i]) == "maps/campaign" + name + ".txt")
			pos = i;
	bool prev = pos > 0, next = pos >= 0 && pos < nfiles - 1;

	std::string sig = name + ":" + std::to_string(progress) + ":" + std::to_string(nfiles) + ":" + std::to_string(pos);
	if (!force && sig == m.signature)
		return;
	m.signature = sig;

	m.levels.clear();
	m.title = "";
	size_t size = 0;
	unsigned char* data = UIE_LoadFile(("maps/campaign" + name + ".txt").c_str(), &size);
	if (data) {
		std::string text((const char*)data, size);
		UIE_Free(data);
		size_t start = 0;
		while (start <= text.size()) {
			size_t end = text.find('\n', start);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(start, end - start);
			start = end + 1;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;
			if (line.compare(0, 11, "//campaign:") == 0) {
				m.title = line.substr(11);
				continue;
			}
			if (line.compare(0, 12, "\"//campaign:") == 0) {
				m.title = line.substr(12, line.size() - 13);
				continue;
			}
			if (line.compare(0, 2, "//") == 0 || line.compare(0, 3, "\"//") == 0)
				continue;
			std::vector<std::string> f = SplitCsv(line);
			if (f.size() < 9)
				continue;
			LevelRow l;
			l.number = (int)m.levels.size() + 1;
			l.mode = Upper(f[0]);
			l.map = f[1];
			l.title = f[7];
			l.desc = ToRml(Replace(f[8], "\\n", "\n"));
			l.image = "/maps/" + f[1];
			int idx = l.number - 1;
			l.done = idx < progress;
			l.current = idx == progress;
			l.locked = idx > progress;
			l.state = l.done ? "done" : l.current ? "current" : "locked";
			if (l.locked) {
				l.title = "Locked";
				l.desc = "";
			}
			m.levels.push_back(l);
		}
	}
	m.name = name;
	m.progress = progress;
	m.selected = std::max(0, std::min(progress, (int)m.levels.size() - 1));
	m.handle.DirtyVariable("selected");
	m.has_prev = prev;
	m.has_next = next;
	m.handle.DirtyVariable("levels");
	m.handle.DirtyVariable("title");
	m.handle.DirtyVariable("has_prev");
	m.handle.DirtyVariable("has_next");
	m.handle.DirtyVariable("progress");
}

void CampaignGo(int step)
{
	char files[16][128];
	int n = UIE_FileList("maps/campaign*.txt", files, 16);
	int pos = -1;
	for (int i = 0; i < n; i++)
		if (std::string(files[i]) == "maps/campaign" + campaign->name + ".txt")
			pos = i;
	int to = pos + step;
	if (n < 1 || to < 0 || to >= n)
		return;
	std::string f = files[to]; // maps/campaign<name>.txt
	UIE_CvarSet("g_campaign_name", f.substr(13, f.size() - 17).c_str());
	CampaignLoad(true);
}

void CampaignUpdate(double now, bool shown)
{
	if (!campaign || !shown || now < campaign->next_update)
		return;
	campaign->next_update = now + 0.5;
	CampaignLoad(false);
}

void CreateCampaign(Rml::Context* context)
{
	Rml::DataModelConstructor c = context->CreateDataModel("campaign");
	if (!c)
		return;
	campaign = new CampaignModel;
	CampaignModel& m = *campaign;
	if (auto s = c.RegisterStruct<LevelRow>()) {
		s.RegisterMember("number", &LevelRow::number);
		s.RegisterMember("title", &LevelRow::title);
		s.RegisterMember("desc", &LevelRow::desc);
		s.RegisterMember("map", &LevelRow::map);
		s.RegisterMember("mode", &LevelRow::mode);
		s.RegisterMember("image", &LevelRow::image);
		s.RegisterMember("state", &LevelRow::state);
		s.RegisterMember("locked", &LevelRow::locked);
		s.RegisterMember("current", &LevelRow::current);
		s.RegisterMember("done", &LevelRow::done);
	}
	c.RegisterArray<std::vector<LevelRow>>();
	c.Bind("levels", &m.levels);
	c.Bind("title", &m.title);
	c.Bind("has_prev", &m.has_prev);
	c.Bind("has_next", &m.has_next);
	c.Bind("progress", &m.progress);
	c.Bind("selected", &m.selected);
	m.handle = c.GetModelHandle();

	c.BindEventCallback("play", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty())
			return;
		int i = args[0].Get<int>();
		if (i < 0 || i >= (int)campaign->levels.size() || campaign->levels[i].locked)
			return;
		// the menu's own code starts the campaign (it keeps the progress and the bots)
		UIE_Command(("menu_cmd campaign " + campaign->name + " " + std::to_string(i)).c_str());
		UIE_Command("rml_hide singleplayer");
	});
	c.BindEventCallback("select", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty())
			return;
		int i = args[0].Get<int>();
		if (i < 0 || i >= (int)campaign->levels.size() || campaign->levels[i].locked)
			return;
		campaign->selected = i;
		h.DirtyVariable("selected");
	});
	c.BindEventCallback("play_selected", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		UIE_Command(("menu_cmd campaign " + campaign->name + " " + std::to_string(campaign->selected)).c_str());
		UIE_Command("rml_hide singleplayer");
	});
	c.BindEventCallback("instant", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		UIE_Command("menu_cmd instantaction");
		UIE_Command("rml_hide singleplayer");
	});
	c.BindEventCallback("next", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { CampaignGo(1); });
	c.BindEventCallback("prev", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { CampaignGo(-1); });
	CampaignLoad(true);
}

// ---------------------------------------------------------------------------
// Settings: screen sizes, key bindings, menu skins
// ---------------------------------------------------------------------------

struct OptionRow {
	std::string label, value;
};

struct BindRow {
	std::string label, command, key1, key2;
	bool header = false, bindable = false;
	bool capturing1 = false, capturing2 = false;
};

struct SettingsModel {
	Rml::DataModelHandle handle;
	std::vector<OptionRow> resolutions, skins;
	std::string resolution;
	std::vector<BindRow> binds;
	std::string capture_text;
	int capture_row = -1, capture_slot = 0;
	double next_update = 0;
};

SettingsModel* settings = nullptr;

void BindsRefresh()
{
	bool changed = false;
	for (BindRow& b : settings->binds) {
		if (b.command.empty())
			continue;
		char k1[32], k2[32];
		UIE_KeysFor(b.command.c_str(), k1, k2, sizeof(k1));
		if (b.key1 != k1 || b.key2 != k2) {
			b.key1 = k1;
			b.key2 = k2;
			changed = true;
		}
	}
	for (size_t i = 0; i < settings->binds.size(); i++) {
		BindRow& b = settings->binds[i];
		bool c1 = (int)i == settings->capture_row && settings->capture_slot == 0;
		bool c2 = (int)i == settings->capture_row && settings->capture_slot == 1;
		if (b.capturing1 != c1 || b.capturing2 != c2) {
			b.capturing1 = c1;
			b.capturing2 = c2;
			changed = true;
		}
	}
	if (changed)
		settings->handle.DirtyVariable("binds");
}

void SettingsUpdate(double now, bool shown)
{
	if (!settings || !shown || now < settings->next_update)
		return;
	settings->next_update = now + 0.3;
	BindsRefresh();
	// the size the cvars describe, as the dropdown's value
	std::string cur = std::string(UIE_CvarString("vid_width")) + "x" + UIE_CvarString("vid_height");
	if (cur != settings->resolution) {
		settings->resolution = cur;
		settings->handle.DirtyVariable("resolution");
	}
}

void CreateSettings(Rml::Context* context)
{
	Rml::DataModelConstructor c = context->CreateDataModel("settings");
	if (!c)
		return;
	settings = new SettingsModel;
	SettingsModel& m = *settings;
	if (auto o = c.RegisterStruct<OptionRow>()) {
		o.RegisterMember("label", &OptionRow::label);
		o.RegisterMember("value", &OptionRow::value);
	}
	c.RegisterArray<std::vector<OptionRow>>();
	if (auto b = c.RegisterStruct<BindRow>()) {
		b.RegisterMember("label", &BindRow::label);
		b.RegisterMember("command", &BindRow::command);
		b.RegisterMember("key1", &BindRow::key1);
		b.RegisterMember("key2", &BindRow::key2);
		b.RegisterMember("header", &BindRow::header);
		b.RegisterMember("bindable", &BindRow::bindable);
		b.RegisterMember("capturing1", &BindRow::capturing1);
		b.RegisterMember("capturing2", &BindRow::capturing2);
	}
	c.RegisterArray<std::vector<BindRow>>();
	c.Bind("resolutions", &m.resolutions);
	c.Bind("resolution", &m.resolution);
	c.Bind("skins", &m.skins);
	c.Bind("binds", &m.binds);
	c.Bind("capture_text", &m.capture_text);
	m.handle = c.GetModelHandle();

	// screen sizes
	UIE_VideoMode modes[64];
	int n = UIE_VideoModes(modes, 64);
	for (int i = 0; i < n; i++) {
		std::string v = std::to_string(modes[i].width) + "x" + std::to_string(modes[i].height);
		m.resolutions.push_back({v, v});
	}
	// menu skins: gfx/menu/<name>/skinvalues.txt
	char files[32][128];
	int nf = UIE_FileList("gfx/menu/*/skinvalues.txt", files, 32);
	for (int i = 0; i < nf; i++) {
		std::string f = files[i];
		size_t a = f.find('/', 4) + 1, b = f.find('/', a);
		if (a != std::string::npos && b != std::string::npos)
			m.skins.push_back({f.substr(a, b - a), f.substr(a, b - a)});
	}

	c.BindEventCallback("resolution_changed", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		size_t x = settings->resolution.find('x');
		if (x == std::string::npos)
			return;
		UIE_CvarSet("vid_width", settings->resolution.substr(0, x).c_str());
		UIE_CvarSet("vid_height", settings->resolution.substr(x + 1).c_str());
	});

	// key bindings: the commands of the game's key binder
	struct { const char *label, *command; } table[] = {
		{"Moving", nullptr},
		{"Move forwards", "+forward"}, {"Move backwards", "+back"}, {"Strafe left", "+moveleft"}, {"Strafe right", "+moveright"},
		{"Jump / swim", "+jump"}, {"Crouch / sink", "+crouch"}, {"Off-hand hook", "+hook"}, {"Jetpack", "+jetpack"},
		{"Attacking", nullptr},
		{"Primary fire", "+fire"}, {"Secondary fire", "+fire2"},
		{"Weapons", nullptr},
		{"Previous weapon", "weapprev"}, {"Next weapon", "weapnext"}, {"Previously used weapon", "weaplast"}, {"Best weapon", "weapbest"},
		{"Reload", "reload"}, {"Drop weapon / throw nade", "dropweapon"},
		{"Weapon group 1", "weapon_group_1"}, {"Weapon group 2", "weapon_group_2"}, {"Weapon group 3", "weapon_group_3"},
		{"Weapon group 4", "weapon_group_4"}, {"Weapon group 5", "weapon_group_5"}, {"Weapon group 6", "weapon_group_6"},
		{"Weapon group 7", "weapon_group_7"}, {"Weapon group 8", "weapon_group_8"}, {"Weapon group 9", "weapon_group_9"},
		{"Weapon group 0", "weapon_group_0"},
		{"Misc", nullptr},
		{"Use (drop flag, leave vehicle)", "+use"}, {"Suicide / respawn", "kill"}, {"Zoom", "+zoom"}, {"Show scoreboard", "+showscores"},
		{"Chat", "messagemode"}, {"Team chat", "messagemode2"}, {"Screenshot", "screenshot"}, {"Quick menu", "quickmenu"},
	};
	for (auto& t : table) {
		BindRow r;
		r.label = t.label;
		if (t.command) {
			r.command = t.command;
			r.bindable = true;
		} else
			r.header = true;
		m.binds.push_back(r);
	}
	c.BindEventCallback("bind", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
		if (args.size() < 2)
			return;
		int row = args[0].Get<int>(), slot = args[1].Get<int>();
		if (row < 0 || row >= (int)settings->binds.size() || !settings->binds[row].bindable)
			return;
		settings->capture_row = row;
		settings->capture_slot = slot;
		settings->capture_text = "Press a key for \"" + settings->binds[row].label + "\"  (Esc cancels)";
		h.DirtyVariable("capture_text");
		UIE_CaptureKeys(1);
		BindsRefresh();
	});
	c.BindEventCallback("unbind", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty())
			return;
		int row = args[0].Get<int>();
		if (row < 0 || row >= (int)settings->binds.size() || !settings->binds[row].bindable)
			return;
		UIE_BindKey(settings->binds[row].command.c_str(), 0, -1);
		UIE_BindKey(settings->binds[row].command.c_str(), 1, -1);
		BindsRefresh();
	});
	BindsRefresh();
}

} // namespace

void UIRml_KeyCaptured(int key)
{
	if (!settings || settings->capture_row < 0)
		return;
	const BindRow& b = settings->binds[settings->capture_row];
	if (key >= 0)
		UIE_BindKey(b.command.c_str(), settings->capture_slot, key);
	settings->capture_row = -1;
	settings->capture_text.clear();
	settings->handle.DirtyVariable("capture_text");
	BindsRefresh();
}

void UIRmlModels_Create(Rml::Context* context)
{
	CreateServers(context);
	CreateCampaign(context);
	CreateSettings(context);
}

void UIRmlModels_Destroy(void)
{
	delete servers;
	servers = nullptr;
	delete campaign;
	campaign = nullptr;
	delete settings;
	settings = nullptr;
}

// shown: whether any document that uses the models is up (the server list
// only asks the network for news while it is looked at)
void UIRmlModels_Update(double now, bool servers_shown, bool campaign_shown, bool settings_shown)
{
	SettingsUpdate(now, settings_shown);
	ServersUpdate(now, servers_shown);
	CampaignUpdate(now, campaign_shown);
}
