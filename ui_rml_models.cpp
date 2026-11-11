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

struct ServerRow {
	std::string address, name, map, type, players;
	int ping = 0;
	int humans = 0;
	int slots = 0;
	int free_slots = 0;
	bool favorite = false;
};

struct ServersModel {
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
};

ServersModel* servers = nullptr;

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
		r.favorite = s.isfavorite != 0;
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
	const std::string key = m.sort;
	std::stable_sort(rows.begin(), rows.end(), [&](const ServerRow& a, const ServerRow& b) {
		int c;
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
	m.status = status;
	m.handle.DirtyVariable("rows");
	m.handle.DirtyVariable("status");
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
	if (!shown || now < servers->next_update)
		return;
	servers->next_update = now + 0.4;
	ServersCvars(true);
	// a refresh is over when nothing has been asking for a while
	ServersRebuild(false);
}

void CreateServers(Rml::Context* context)
{
	Rml::DataModelConstructor c = context->CreateDataModel("servers");
	if (!c)
		return;
	servers = new ServersModel;
	ServersModel& m = *servers;
	if (auto s = c.RegisterStruct<ServerRow>()) {
		s.RegisterMember("address", &ServerRow::address);
		s.RegisterMember("name", &ServerRow::name);
		s.RegisterMember("map", &ServerRow::map);
		s.RegisterMember("type", &ServerRow::type);
		s.RegisterMember("players", &ServerRow::players);
		s.RegisterMember("ping", &ServerRow::ping);
		s.RegisterMember("favorite", &ServerRow::favorite);
	}
	c.RegisterArray<std::vector<ServerRow>>();
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
		h.DirtyVariable("selected");
		h.DirtyVariable("address");
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

} // namespace

void UIRmlModels_Create(Rml::Context* context)
{
	CreateServers(context);
	CreateCampaign(context);
}

void UIRmlModels_Destroy(void)
{
	delete servers;
	servers = nullptr;
	delete campaign;
	campaign = nullptr;
}

// shown: whether any document that uses the models is up (the server list
// only asks the network for news while it is looked at)
void UIRmlModels_Update(double now, bool servers_shown, bool campaign_shown)
{
	ServersUpdate(now, servers_shown);
	CampaignUpdate(now, campaign_shown);
}
