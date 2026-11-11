/*
 * Insanotic: RmlUi in the engine, C++ side.
 *
 * RmlUi lays out and styles HTML-like documents (RML and RCSS files, see
 * data/insanotic.pk3dir/ui/). This file gives it what it needs from the
 * engine: a renderer that draws through DrawQ, a file system that reads from
 * the engine's pk3-aware one and a clock. It also owns the loaded documents
 * and turns the engine's input into RmlUi's.
 *
 * Buttons in a document run console commands: an element with a data-cmd
 * attribute queues that command when it is clicked or tapped. An element with
 * a data-cvar attribute shows that cvar's value as its text. There is no
 * scripting, on purpose.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <RmlUi/Core.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui_rml.h"

// data models for the lists (ui_rml_models.cpp)
void UIRmlModels_Create(Rml::Context* context);
void UIRmlModels_Destroy(void);
void UIRmlModels_Update(double now, bool servers_shown, bool campaign_shown, bool settings_shown);

namespace {

// ---------------------------------------------------------------------------
// File interface: everything comes through the engine's file system, so
// documents, stylesheets, fonts and images can sit in a pk3 as well as in a
// directory. A file is read whole into memory, which is what fonts want
// anyway and is cheap for the rest.
// ---------------------------------------------------------------------------

struct MemFile {
	unsigned char* data = nullptr;
	size_t size = 0;
	size_t pos = 0;
};

class EngineFiles : public Rml::FileInterface {
public:
	Rml::FileHandle Open(const Rml::String& path) override
	{
		// the engine's paths never start with a slash
		const char* p = path.c_str();
		while (*p == '/')
			++p;
		// nothing outside the game's folders, whoever asks (pages from a
		// server name their own files)
		if (strstr(p, "..") || strchr(p, ':') || strchr(p, '\\'))
			return 0;
		size_t size = 0;
		unsigned char* data = UIE_LoadFile(p, &size);
		if (!data)
			return 0;
		MemFile* f = new MemFile;
		f->data = data;
		f->size = size;
		return reinterpret_cast<Rml::FileHandle>(f);
	}
	void Close(Rml::FileHandle file) override
	{
		MemFile* f = reinterpret_cast<MemFile*>(file);
		UIE_Free(f->data);
		delete f;
	}
	size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
	{
		MemFile* f = reinterpret_cast<MemFile*>(file);
		size_t n = f->size - f->pos;
		if (size < n)
			n = size;
		if (n)
			memcpy(buffer, f->data + f->pos, n);
		f->pos += n;
		return n;
	}
	bool Seek(Rml::FileHandle file, long offset, int origin) override
	{
		MemFile* f = reinterpret_cast<MemFile*>(file);
		long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? (long)f->pos : (long)f->size;
		long to = base + offset;
		if (to < 0 || to > (long)f->size)
			return false;
		f->pos = (size_t)to;
		return true;
	}
	size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<MemFile*>(file)->pos; }
	size_t Length(Rml::FileHandle file) override { return reinterpret_cast<MemFile*>(file)->size; }
};

// ---------------------------------------------------------------------------
// System interface: clock, log, on-screen keyboard
// ---------------------------------------------------------------------------

class EngineSystem : public Rml::SystemInterface {
public:
	double GetElapsedTime() override { return UIE_Time(); }
	bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
	{
		int level = 0;
		switch (type) {
		case Rml::Log::LT_ERROR:
		case Rml::Log::LT_ASSERT:
		case Rml::Log::LT_WARNING:
			level = 1;
			break;
		case Rml::Log::LT_INFO:
		default:
			level = 2;
			break;
		}
		UIE_Log(level, message.c_str());
		return true;
	}
	void ActivateKeyboard(Rml::Vector2f, float) override { UIE_Keyboard(1); }
	void DeactivateKeyboard() override { UIE_Keyboard(0); }
};

// ---------------------------------------------------------------------------
// Render interface: geometry is kept on our side and handed to the engine's
// 2D drawing for every draw call. The menu is a few hundred triangles, so
// there is nothing to gain from keeping it in GPU buffers.
// ---------------------------------------------------------------------------

struct Geometry {
	std::vector<float> xy, uv, rgba;
	std::vector<int> indices;
};

struct Texture {
	void* handle;
};

class EngineRender : public Rml::RenderInterface {
public:
	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
	{
		Geometry* g = new Geometry;
		size_t n = vertices.size();
		g->xy.resize(n * 2);
		g->uv.resize(n * 2);
		g->rgba.resize(n * 4);
		for (size_t i = 0; i < n; ++i) {
			const Rml::Vertex& v = vertices[i];
			g->xy[i * 2] = v.position.x;
			g->xy[i * 2 + 1] = v.position.y;
			g->uv[i * 2] = v.tex_coord.x;
			g->uv[i * 2 + 1] = v.tex_coord.y;
			g->rgba[i * 4] = v.colour.red / 255.0f;
			g->rgba[i * 4 + 1] = v.colour.green / 255.0f;
			g->rgba[i * 4 + 2] = v.colour.blue / 255.0f;
			g->rgba[i * 4 + 3] = v.colour.alpha / 255.0f;
		}
		g->indices.assign(indices.begin(), indices.end());
		return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
	}

	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override
	{
		const Geometry* g = reinterpret_cast<const Geometry*>(geometry);
		if (g->indices.empty())
			return;
		size_t n = g->xy.size() / 2;
		scratch_.resize(n * 2);
		for (size_t i = 0; i < n; ++i) {
			float x = g->xy[i * 2] + translation.x;
			float y = g->xy[i * 2 + 1] + translation.y;
			if (has_transform_) {
				// RmlUi hands a full 4x4 matrix for transform properties; the
				// result is in window pixels, after the perspective divide
				Rml::Vector4f p = transform_ * Rml::Vector4f(x, y, 0.0f, 1.0f);
				float w = p.w != 0.0f ? p.w : 1.0f;
				x = p.x / w;
				y = p.y / w;
			}
			scratch_[i * 2] = x;
			scratch_[i * 2 + 1] = y;
		}
		const Texture* t = reinterpret_cast<const Texture*>(texture);
		UIE_DrawTriangles(scratch_.data(), g->uv.data(), g->rgba.data(), (int)n, g->indices.data(), (int)g->indices.size(), t ? t->handle : nullptr);
	}

	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override { delete reinterpret_cast<Geometry*>(geometry); }

	Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
	{
		int w = 0, h = 0;
		unsigned char* pixels = UIE_LoadImage(source.c_str(), &w, &h);
		if (!pixels)
			return 0;
		void* handle = UIE_TextureCreate(pixels, w, h, 1);
		UIE_Free(pixels);
		if (!handle)
			return 0;
		dimensions.x = w;
		dimensions.y = h;
		return reinterpret_cast<Rml::TextureHandle>(new Texture{handle});
	}

	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override
	{
		void* handle = UIE_TextureCreate(source.data(), dimensions.x, dimensions.y, 0);
		if (!handle)
			return 0;
		return reinterpret_cast<Rml::TextureHandle>(new Texture{handle});
	}

	void ReleaseTexture(Rml::TextureHandle texture) override
	{
		Texture* t = reinterpret_cast<Texture*>(texture);
		UIE_TextureFree(t->handle);
		delete t;
	}

	void EnableScissorRegion(bool enable) override
	{
		scissor_on_ = enable;
		ApplyScissor();
	}

	void SetScissorRegion(Rml::Rectanglei region) override
	{
		scissor_ = region;
		ApplyScissor();
	}

	void SetTransform(const Rml::Matrix4f* transform) override
	{
		has_transform_ = transform != nullptr;
		if (transform)
			transform_ = *transform;
	}

private:
	void ApplyScissor() { UIE_Scissor(scissor_on_, scissor_.Left(), scissor_.Top(), scissor_.Width(), scissor_.Height()); }

	bool scissor_on_ = false;
	Rml::Rectanglei scissor_ = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
	bool has_transform_ = false;
	Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
	std::vector<float> scratch_;
};

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

// What a click on an element can do. The attributes are looked up from the
// element that was hit upwards, so a button may have children.
//   data-cmd="..."    run that console command (documents of the game only);
//   data-close        close the document the element is in;
//   data-reply="act"  server pages: send the page's answer to the server.
// The rules differ for pages that come from a server, see ActionListener.
class ActionListener : public Rml::EventListener {
public:
	void ProcessEvent(Rml::Event& event) override;
};

// two-way cvar binding: a form control with data-cvar writes its value to the
// cvar when the player changes it
class CvarWriter : public Rml::EventListener {
public:
	void ProcessEvent(Rml::Event& event) override;
};

struct Document {
	Rml::ElementDocument* doc = nullptr;
	bool shown = false;
	bool server = false; // a page a server sent: it is not trusted, see UIRml_ShowServer
	double shown_at = 0;
	std::string path;    // server pages: where it was loaded from
	std::map<std::string, bool> classes;
};

struct State {
	EngineFiles files;
	EngineSystem system;
	EngineRender render;
	ActionListener actions;
	CvarWriter writer;
	std::vector<std::string> pending_close; // documents to close at the start of the next frame
	double last_server_open = 0;
	Rml::Context* context = nullptr;
	bool initialised = false;
	std::map<std::string, Document> documents;
	std::unordered_map<Rml::Element*, std::string> bound; // data-cvar elements and the text they show
	double next_binding = 0;

	// drag scrolling
	bool pressed = false;
	bool touch = false;
	bool touch_mode = false; // the input is a finger: no hover, only the pressed state
	bool left_target = false; // the finger went down on something and is now off it
	Rml::Element* press_target = nullptr;
	bool scrolling = false;
	Rml::Vector2f press_pos, last_pos;
	Rml::Element* scroll_target = nullptr;
	Rml::Vector2f velocity; // pixels per second, for the fling after a drag
	// Where the content would be if scrolling were not stopped at the ends.
	// The part beyond the ends is drawn as the rubber band and springs back.
	Rml::Vector2f pos;
	Rml::Vector2f shown_offset; // the rubber band offset set on the children now
	bool swallow = false; // this press only stopped a moving list: it is not a click
	double last_move_time = 0;
	double last_frame_time = 0;
	float dpratio = 1.0f;
};

State* g = nullptr;

// "server:ui/x/y.rml" is a page from a server, everything else is
// ui/<name>.rml of the game
const char* const SERVER_PREFIX = "server:";
bool IsServerName(const std::string& name) { return name.compare(0, 7, SERVER_PREFIX) == 0; }
std::string DocumentPath(const std::string& name)
{
	if (IsServerName(name))
		return name.substr(7);
	return "ui/" + name + ".rml";
}

Document* DocumentOf(Rml::Element* e)
{
	Rml::ElementDocument* doc = e ? e->GetOwnerDocument() : nullptr;
	for (auto& kv : g->documents)
		if (kv.second.doc == doc && doc)
			return &kv.second;
	return nullptr;
}

std::string DocumentName(Document* d)
{
	for (auto& kv : g->documents)
		if (&kv.second == d)
			return kv.first;
	return "";
}

// A value that goes into a command line the server splits into words: no
// quotes, backslashes or control characters, and not very long.
std::string Sanitize(const Rml::String& in, size_t max)
{
	std::string out;
	for (unsigned char c : in) {
		if (out.size() >= max)
			break;
		if (c < 32 || c == '"' || c == '\\' || c == 127)
			continue;
		out += (char)c;
	}
	return out;
}

// the page's answer: "rml_reply <page> <action> [name value]...", with
// every named control of the page. At most 16 controls and about a kilobyte.
std::string BuildReply(Document* d, const Rml::String& action)
{
	std::string line = "rml_reply \"" + Sanitize(d->path, 120) + "\" \"" + Sanitize(action, 60) + "\"";
	Rml::ElementList list;
	d->doc->QuerySelectorAll(list, "input, select, textarea");
	int n = 0;
	for (Rml::Element* e : list) {
		if (!e->HasAttribute("name") || n >= 16 || line.size() > 900)
			continue;
		std::string value;
		const Rml::String type = e->GetAttribute<Rml::String>("type", "text");
		if (e->GetTagName() == "input" && type == "checkbox") {
			value = e->HasAttribute("checked") ? "1" : "0";
		} else if (e->GetTagName() == "input" && type == "radio") {
			if (!e->HasAttribute("checked"))
				continue;
			value = Sanitize(e->GetAttribute<Rml::String>("value", ""), 80);
		} else if (auto* fc = rmlui_dynamic_cast<Rml::ElementFormControl*>(e)) {
			value = Sanitize(fc->GetValue(), 200);
		} else {
			continue;
		}
		line += " \"" + Sanitize(e->GetAttribute<Rml::String>("name", ""), 40) + "\" \"" + value + "\"";
		n++;
	}
	return line;
}



// A drag that starts on something scrollable scrolls it instead of pressing a
// button. Walks up from what is under the finger.
Rml::Element* FindScrollable(Rml::Element* e)
{
	for (; e; e = e->GetParentNode()) {
		const auto& cv = e->GetComputedValues();
		using O = Rml::Style::Overflow;
		bool y = (cv.overflow_y() == O::Scroll || cv.overflow_y() == O::Auto) && e->GetScrollHeight() > e->GetClientHeight() + 1.0f;
		bool x = (cv.overflow_x() == O::Scroll || cv.overflow_x() == O::Auto) && e->GetScrollWidth() > e->GetClientWidth() + 1.0f;
		if (x || y)
			return e;
	}
	return nullptr;
}

// a finger on a slider moves the slider, not the list it is in
bool OnDragControl(Rml::Element* e)
{
	for (; e; e = e->GetParentNode()) {
		const Rml::String& t = e->GetTagName();
		if (t == "sliderbar" || t == "slidertrack" || t == "sliderprogress")
			return true;
	}
	return false;
}

Rml::Vector2f ScrollMax(Rml::Element* e)
{
	return Rml::Vector2f(std::max(0.0f, e->GetScrollWidth() - e->GetClientWidth()),
	                     std::max(0.0f, e->GetScrollHeight() - e->GetClientHeight()));
}

// The farther a list is pulled past its end, the less it follows the finger,
// like a stretched rubber band. It never exceeds the size of the list.
float Rubber(float excess, float size)
{
	size = std::max(size, 1.0f);
	float a = std::fabs(excess);
	float r = (1.0f - 1.0f / (a * 0.55f / size + 1.0f)) * size;
	return excess < 0 ? -r : r;
}

// Shows g->pos: the part inside the list scrolls it, the part beyond the
// ends moves the content (RmlUi cannot scroll past the ends, so the children
// are shifted instead; the scroll bars stay where they are).
void ApplyScroll(Rml::Element* e)
{
	Rml::Vector2f max = ScrollMax(e);
	Rml::Vector2f in(std::min(std::max(g->pos.x, 0.0f), max.x), std::min(std::max(g->pos.y, 0.0f), max.y));
	e->SetScrollLeft(in.x);
	e->SetScrollTop(in.y);
	Rml::Vector2f off(-Rubber(g->pos.x - in.x, e->GetClientWidth()), -Rubber(g->pos.y - in.y, e->GetClientHeight()));
	if (off.x == g->shown_offset.x && off.y == g->shown_offset.y)
		return;
	g->shown_offset = off;
	for (int i = 0; i < e->GetNumChildren(); i++) {
		Rml::Element* c = e->GetChild(i);
		if (off.x == 0 && off.y == 0)
			c->RemoveProperty("transform");
		else
			c->SetProperty("transform", "translate(" + std::to_string(off.x) + "px, " + std::to_string(off.y) + "px)");
	}
}

void EndScroll()
{
	g->scroll_target = nullptr;
	g->velocity = Rml::Vector2f(0, 0);
	g->shown_offset = Rml::Vector2f(0, 0);
	g->swallow = false;
}

void ActionListener::ProcessEvent(Rml::Event& event)
{
	// the click may land on a child of the element that carries the attribute
	for (Rml::Element* e = event.GetCurrentElement(); e; e = e->GetParentNode()) {
		bool close = e->HasAttribute("data-close");
		bool reply = e->HasAttribute("data-reply");
		bool cmd = e->HasAttribute("data-cmd");
		if (!close && !reply && !cmd)
			continue;
		Document* d = DocumentOf(e);
		if (!d)
			return;
		if (d->server) {
			// A page from a server can only talk to that server: its answer,
			// or a "cmd ..." line for the server's own game code. Nothing it
			// writes runs on this computer.
			if (close) {
				g->pending_close.push_back(DocumentName(d));
			} else if (reply) {
				UIE_ServerCommand(BuildReply(d, e->GetAttribute<Rml::String>("data-reply", "")).c_str());
			} else {
				Rml::String c = e->GetAttribute<Rml::String>("data-cmd", "");
				if (c.compare(0, 4, "cmd ") == 0 && c.size() < 200)
					UIE_ServerCommand(Sanitize(c.substr(4), 190).c_str());
				else
					UIE_Log(1, "a page from the server asked for a local command; refused");
			}
		} else {
			// a button may run a command and close its dialog
			if (cmd) {
				Rml::String c = e->GetAttribute<Rml::String>("data-cmd", "");
				if (!c.empty())
					UIE_Command(c.c_str());
			}
			if (close)
				g->pending_close.push_back(DocumentName(d));
		}
		return;
	}
}

// Same text for a cvar and a control? Numbers compare as numbers, so "1" and
// "1.000000" match.
bool SameValue(const Rml::String& a, const Rml::String& b)
{
	if (a == b)
		return true;
	char *ea, *eb;
	double x = strtod(a.c_str(), &ea), y = strtod(b.c_str(), &eb);
	return !a.empty() && !b.empty() && !*ea && !*eb && std::fabs(x - y) < 1e-6;
}

// Controls can say how their value relates to the cvar's:
//   data-on / data-off   a checkbox writes these instead of 1 and 0 (it is
//                        checked while the cvar is not the off value);
//   data-map="db"        a slider in decibels for a volume cvar (the cvar is
//                        a factor; the slider's lowest value is silence).
Rml::String FormatNumber(double v)
{
	char buf[48];
	snprintf(buf, sizeof(buf), "%.6g", v);
	return buf;
}

Rml::String ControlFromCvar(Rml::Element* e, const Rml::String& value)
{
	if (e->GetAttribute<Rml::String>("data-map", "") == "db") {
		double v = atof(value.c_str());
		double lo = e->GetAttribute<double>("min", -40.0);
		double db = v <= 0 ? lo : 20.0 * std::log10(v);
		return FormatNumber(std::max(lo, std::min(db, e->GetAttribute<double>("max", 0.0))));
	}
	return value;
}

Rml::String CvarFromControl(Rml::Element* e, const Rml::String& value)
{
	if (e->GetAttribute<Rml::String>("data-map", "") == "db") {
		double db = atof(value.c_str());
		if (db <= e->GetAttribute<double>("min", -40.0) + 1e-6)
			return "0";
		return FormatNumber(std::pow(10.0, db / 20.0));
	}
	return value;
}

Rml::ElementFormControl* AsControl(Rml::Element* e) { return rmlui_dynamic_cast<Rml::ElementFormControl*>(e); }

// the cvar changed (or the document was just shown): bring the control in line
void SyncControl(Rml::Element* e, const Rml::String& value)
{
	const Rml::String& tag = e->GetTagName();
	if (tag == "input") {
		const Rml::String type = e->GetAttribute<Rml::String>("type", "text");
		if (type == "checkbox") {
			bool on = !value.empty() && !SameValue(value, e->GetAttribute<Rml::String>("data-off", "0"));
			if (on != e->HasAttribute("checked")) {
				if (on)
					e->SetAttribute("checked", true);
				else
					e->RemoveAttribute("checked");
			}
			return;
		}
		if (type == "radio") {
			bool on = SameValue(value, e->GetAttribute<Rml::String>("value", ""));
			if (on != e->HasAttribute("checked")) {
				if (on)
					e->SetAttribute("checked", true);
				else
					e->RemoveAttribute("checked");
			}
			return;
		}
	}
	// a field being typed in is the player's, not the cvar's
	if (e == g->context->GetFocusElement())
		return;
	if (Rml::ElementFormControl* fc = AsControl(e)) {
		Rml::String shown = ControlFromCvar(e, value);
		if (!SameValue(fc->GetValue(), shown))
			fc->SetValue(shown);
		// a dropdown whose options are made from data may not have the
		// option yet: look again at the next check
		if (e->GetTagName() == "select" && !SameValue(fc->GetValue(), shown))
			g->bound.erase(e);
	}
}

void CvarWriter::ProcessEvent(Rml::Event& event)
{
	Rml::Element* e = event.GetTargetElement();
	while (e && !e->HasAttribute("data-cvar"))
		e = e->GetParentNode();
	Document* d = DocumentOf(e);
	if (!e || !d || d->server)
		return;
	Rml::String value;
	const Rml::String type = e->GetAttribute<Rml::String>("type", "text");
	if (e->GetTagName() == "input" && type == "checkbox") {
		value = e->HasAttribute("checked") ? e->GetAttribute<Rml::String>("data-on", "1") : e->GetAttribute<Rml::String>("data-off", "0");
	} else if (e->GetTagName() == "input" && type == "radio") {
		if (!e->HasAttribute("checked"))
			return;
		value = e->GetAttribute<Rml::String>("value", "");
	} else if (Rml::ElementFormControl* fc = AsControl(e)) {
		value = CvarFromControl(e, fc->GetValue());
	} else {
		return;
	}
	Rml::String name = e->GetAttribute<Rml::String>("data-cvar", "");
	if (SameValue(UIE_CvarString(name.c_str()), value))
		return;
	UIE_CvarSet(name.c_str(), value.c_str());
	g->bound[e] = UIE_CvarString(name.c_str());
}

void PrepareDocument(Document& d)
{
	Rml::ElementList list;
	d.doc->QuerySelectorAll(list, "[data-cmd], [data-close], [data-reply]");
	for (Rml::Element* e : list)
		e->AddEventListener("click", &g->actions);
	if (!d.server) {
		list.clear();
		d.doc->QuerySelectorAll(list, "input[data-cvar], select[data-cvar], textarea[data-cvar]");
		for (Rml::Element* e : list)
			e->AddEventListener("change", &g->writer);
	}
	for (const auto& c : d.classes)
		if (Rml::Element* body = d.doc)
			body->SetClass(c.first, c.second);
}

// Elements that show a cvar (data-cvar): a plain element shows its value as
// text, a form control shows it and writes changes back (CvarWriter).
// Checked a few times a second, not every frame. Pages from a server are not
// bound at all: they must not read the player's settings.
void UpdateBindings()
{
	double now = UIE_Time();
	if (now < g->next_binding)
		return;
	g->next_binding = now + 0.25;
	for (auto& kv : g->documents) {
		if (!kv.second.shown || kv.second.server)
			continue;
		Rml::ElementList list;
		kv.second.doc->QuerySelectorAll(list, "[data-cvar]");
		for (Rml::Element* e : list) {
			Rml::String value = UIE_CvarString(e->GetAttribute<Rml::String>("data-cvar", "").c_str());
			auto it = g->bound.find(e);
			if (it != g->bound.end() && it->second == value)
				continue;
			g->bound[e] = value;
			const Rml::String& tag = e->GetTagName();
			if (tag == "input" || tag == "select" || tag == "textarea") {
				SyncControl(e, value);
				continue;
			}
			Rml::String escaped;
			for (char c : value) {
				switch (c) {
				case '<': escaped += "&lt;"; break;
				case '>': escaped += "&gt;"; break;
				case '&': escaped += "&amp;"; break;
				default: escaped += c; break;
				}
			}
			e->SetInnerRML(escaped);
		}
	}
}

// takes a document away for good (pages from a server are made again on the
// next show)
void CloseDocument(const std::string& name)
{
	auto it = g->documents.find(name);
	if (it == g->documents.end())
		return;
	Document& d = it->second;
	if (!d.doc || !d.server) {
		if (d.doc && d.shown) {
			d.doc->Hide();
			d.shown = false;
		}
	} else {
		if (g->scroll_target && g->scroll_target->GetOwnerDocument() == d.doc)
			EndScroll();
		for (auto b = g->bound.begin(); b != g->bound.end();)
			b = b->first->GetOwnerDocument() == d.doc ? g->bound.erase(b) : std::next(b);
		g->context->UnloadDocument(d.doc);
		g->documents.erase(it);
	}
	g->pressed = false;
	g->scrolling = false;
}

void LoadFontIfPresent(const char* path, bool fallback)
{
	size_t size = 0;
	unsigned char* probe = UIE_LoadFile(path, &size);
	if (!probe)
		return;
	UIE_Free(probe);
	Rml::LoadFontFace(path, fallback);
}

} // namespace

// ---------------------------------------------------------------------------
// The C interface
// ---------------------------------------------------------------------------

extern "C" {

int UIRml_Create(int width, int height, float dpratio)
{
	if (g)
		return 1;
	g = new State;
	Rml::SetFileInterface(&g->files);
	Rml::SetSystemInterface(&g->system);
	Rml::SetRenderInterface(&g->render);
	if (!Rml::Initialise()) {
		UIE_Log(1, "RmlUi did not start");
		delete g;
		g = nullptr;
		return 0;
	}
	g->initialised = true;
	g->context = Rml::CreateContext("main", Rml::Vector2i(width, height));
	if (!g->context) {
		UIE_Log(1, "RmlUi could not make its context");
		Rml::Shutdown();
		delete g;
		g = nullptr;
		return 0;
	}
	g->context->SetDensityIndependentPixelRatio(dpratio);
	g->dpratio = dpratio;
	// the wheel glides to its target instead of jumping
	g->context->SetDefaultScrollBehavior(Rml::ScrollBehavior::Smooth, 1.0f);
	UIRmlModels_Create(g->context);

	// Xolonium is what the rest of the menu is set in. The names it answers
	// to in RCSS (font-family) come from the font files.
	LoadFontIfPresent("ui/fonts/xolonium-regular.otf", false);
	LoadFontIfPresent("ui/fonts/xolonium-bold.otf", false);
	// DejaVu carries the letters Xolonium lacks (Cyrillic, Greek)
	LoadFontIfPresent("fonts/DejaVuSans-Bold.ttf", true);
	return 1;
}

void UIRml_Destroy(void)
{
	if (!g)
		return;
	g->bound.clear();
	g->documents.clear();
	if (g->initialised)
		Rml::Shutdown();
	UIRmlModels_Destroy();
	delete g;
	g = nullptr;
}

void UIRml_Resize(int width, int height, float dpratio)
{
	if (!g)
		return;
	g->context->SetDimensions(Rml::Vector2i(width, height));
	if (dpratio != g->dpratio) {
		g->dpratio = dpratio;
		g->context->SetDensityIndependentPixelRatio(dpratio);
	}
}

int UIRml_Show(const char* name)
{
	if (!g)
		return 0;
	// only UIRml_ShowServer makes pages from a server, with their limits
	if (IsServerName(name) || strstr(name, ".."))
		return 0;
	Document& d = g->documents[name];
	if (!d.doc) {
		d.doc = g->context->LoadDocument(DocumentPath(name));
		if (!d.doc) {
			g->documents.erase(name);
			return 0;
		}
		PrepareDocument(d);
	}
	if (!d.shown) {
		d.doc->Show();
		d.shown = true;
	}
	return 1;
}

void UIRml_Hide(const char* name)
{
	if (!g)
		return;
	auto it = g->documents.find(name);
	if (it == g->documents.end() || !it->second.shown)
		return;
	CloseDocument(name);
}

void UIRml_HideAll(void)
{
	if (!g)
		return;
	std::vector<std::string> names;
	for (auto& kv : g->documents)
		if (kv.second.shown)
			names.push_back(kv.first);
	for (const std::string& n : names)
		CloseDocument(n);
	g->pressed = false;
	g->scrolling = false;
}

// Does the file, or a template it links, bind a data model of the game? RmlUi
// takes the data-model attribute off while it loads, so it is looked for in
// the text beforehand.
bool NamesDataModel(const std::string& path, int depth)
{
	size_t size = 0;
	unsigned char* data = UIE_LoadFile(path.c_str(), &size);
	if (!data)
		return false;
	std::string text((const char*)data, size);
	UIE_Free(data);
	for (char& c : text)
		c = (char)tolower((unsigned char)c);
	if (text.find("data-model") != std::string::npos)
		return true;
	if (depth > 3)
		return true; // templates that link templates without end
	std::string dir = path.substr(0, path.rfind('/') + 1);
	for (size_t p = 0; (p = text.find("href=\"", p)) != std::string::npos;) {
		p += 6;
		size_t q = text.find('"', p);
		if (q == std::string::npos)
			break;
		std::string ref = text.substr(p, q - p);
		p = q;
		if (ref.size() > 4 && ref.compare(ref.size() - 4, 4, ".rml") == 0) {
			std::string full = ref[0] == '/' ? ref.substr(1) : dir + ref;
			if (NamesDataModel(full, depth + 1))
				return true;
		}
	}
	return false;
}

// A page a server sent. It is the one kind of document that is not ours, so
// it is held to limits and loses abilities (see the listeners above): at most
// one at a time, a file of at most 256 KB, at most 2000 elements, a path in
// ui/ with a folder of its own, no cvar bindings and no local commands.
const char* UIRml_ShowServer(const char* path)
{
	if (!g)
		return "the UI is not running";
	size_t size = 0;
	unsigned char* probe = UIE_LoadFile(path, &size);
	if (!probe)
		return "the page is not in the files the server sent";
	UIE_Free(probe);
	if (size > 256 * 1024)
		return "the page is too large";
	if (NamesDataModel(path, 0))
		return "the page asks for data of the game";
	// one at a time: a new page replaces the one before
	std::vector<std::string> old;
	for (auto& kv : g->documents)
		if (kv.second.server)
			old.push_back(kv.first);
	for (const std::string& n : old)
		CloseDocument(n);

	std::string name = std::string(SERVER_PREFIX) + path;
	Document& d = g->documents[name];
	d.server = true;
	d.path = path;
	d.doc = g->context->LoadDocument(path);
	if (!d.doc) {
		g->documents.erase(name);
		return "the page did not load";
	}
	int count = 0;
	std::vector<Rml::Element*> todo{d.doc};
	while (!todo.empty() && count <= 2000) {
		Rml::Element* e = todo.back();
		todo.pop_back();
		count++;
		for (int i = 0; i < e->GetNumChildren(true); i++)
			todo.push_back(e->GetChild(i));
	}
	if (count > 2000) {
		g->context->UnloadDocument(d.doc);
		g->documents.erase(name);
		return "the page has too many elements";
	}
	// the lists of the game (servers, campaign) can start a connection or a
	// map: not for a page the server sent
	Rml::ElementList models;
	d.doc->QuerySelectorAll(models, "[data-model]");
	if (!models.empty()) {
		g->context->UnloadDocument(d.doc);
		g->documents.erase(name);
		return "the page asks for data of the game";
	}
	PrepareDocument(d);
	d.doc->Show();
	d.shown = true;
	d.shown_at = UIE_Time();
	return nullptr;
}

int UIRml_ServerShown(void)
{
	if (!g)
		return 0;
	for (auto& kv : g->documents)
		if (kv.second.server && kv.second.shown)
			return 1;
	return 0;
}

int UIRml_IsShown(const char* name)
{
	if (!g)
		return 0;
	auto it = g->documents.find(name);
	return it != g->documents.end() && it->second.shown;
}

int UIRml_AnyShownExcept(const char* name)
{
	if (!g)
		return 0;
	for (auto& kv : g->documents)
		if (kv.second.shown && kv.first != name)
			return 1;
	return 0;
}

void UIRml_Reload(void)
{
	if (!g)
		return;
	g->bound.clear();
	g->scroll_target = nullptr;
	g->pressed = g->scrolling = false;
	// loaded stylesheets and images are cached by path, which is what a
	// designer editing them does not want
	Rml::Factory::ClearStyleSheetCache();
	Rml::Factory::ClearTemplateCache();
	Rml::ReleaseTextures();
	std::vector<std::string> servers;
	for (auto& kv : g->documents)
		if (kv.second.server)
			servers.push_back(kv.first);
	for (const std::string& n : servers)
		CloseDocument(n);
	for (auto& kv : g->documents) {
		Document& d = kv.second;
		if (d.doc)
			g->context->UnloadDocument(d.doc);
		d.doc = g->context->LoadDocument(DocumentPath(kv.first));
		if (!d.doc) {
			d.shown = false;
			continue;
		}
		PrepareDocument(d);
		if (d.shown)
			d.doc->Show();
	}
}

void UIRml_SetBodyClass(const char* name, const char* cls, int on)
{
	if (!g)
		return;
	auto it = g->documents.find(name);
	if (it == g->documents.end())
		return;
	it->second.classes[cls] = on != 0;
	if (it->second.doc)
		it->second.doc->SetClass(cls, on != 0);
}

void UIRml_List(void (*print)(const char* line))
{
	if (!g)
		return;
	for (auto& kv : g->documents) {
		std::string line = kv.first + (kv.second.shown ? " (shown)" : (kv.second.doc ? " (hidden)" : " (failed to load)"));
		print(line.c_str());
	}
}

void UIRml_State(const char* id, void (*print)(const char* line))
{
	if (!g)
		return;
	for (auto& kv : g->documents) {
		if (!kv.second.shown)
			continue;
		if (Rml::Element* e = kv.second.doc->GetElementById(id)) {
			char line[160];
			snprintf(line, sizeof(line), "%s hover=%d active=%d", id, e->IsPseudoClassSet("hover") ? 1 : 0, e->IsPseudoClassSet("active") ? 1 : 0);
			print(line);
			return;
		}
	}
	print("no such element");
}

void UIRml_EachShown(void (*each)(const char* name))
{
	if (!g)
		return;
	for (auto& kv : g->documents)
		if (kv.second.shown)
			each(kv.first.c_str());
}

// A finger does not hover. RmlUi sets :hover on what is under the pointer, so
// on a touch screen it is taken off again right away; the pressed state
// (:active) is the only feedback, and it is given by the element where the
// finger went down, for as long as the finger is still on it.
static void ClearHover()
{
	for (Rml::Element* e = g->context->GetHoverElement(); e; e = e->GetParentNode())
		e->SetPseudoClass("hover", false);
}

static void SetPressed(bool on)
{
	for (Rml::Element* e = g->press_target; e; e = e->GetParentNode())
		e->SetPseudoClass("active", on);
}

void UIRml_SetTouch(int touch)
{
	if (g)
		g->touch_mode = touch != 0;
}

void UIRml_MouseMove(float x, float y)
{
	if (!g)
		return;
	if (g->pressed && g->touch) {
		Rml::Vector2f now(x, y);
		if (!g->scrolling) {
			// a finger moves a little even when it means a tap
			float slop = 10.0f * g->dpratio;
			Rml::Vector2f d = now - g->press_pos;
			if (d.x * d.x + d.y * d.y > slop * slop) {
				// a list that is still moving from the last flick keeps being the one dragged
				if (!(g->swallow && g->scroll_target))
					g->scroll_target = OnDragControl(g->context->GetHoverElement()) ? nullptr : FindScrollable(g->context->GetHoverElement());
				if (g->scroll_target) {
					if (!g->swallow)
						g->pos = Rml::Vector2f(g->scroll_target->GetScrollLeft(), g->scroll_target->GetScrollTop());
					g->scrolling = true;
					// take the press back: the element under the finger must not click
					g->context->ProcessMouseLeave();
					SetPressed(false);
					g->last_pos = g->press_pos;
				}
			}
		}
		if (!g->scrolling) {
			// The pointer stays where the finger went down: dragging over
			// other buttons must not light them or let them be clicked.
			// Off the pressed element the press is let go of (no click
			// when the finger lifts there); back on it, it is held again.
			bool inside = !g->press_target || g->press_target->IsPointWithinElement(now);
			if (!inside && !g->left_target) {
				g->context->ProcessMouseLeave();
				SetPressed(false);
				g->left_target = true;
			} else if (inside && g->left_target) {
				g->context->ProcessMouseMove((int)std::lround(x), (int)std::lround(y), 0);
				ClearHover();
				SetPressed(true);
				g->left_target = false;
			}
			g->last_pos = now;
			return;
		}
		if (g->scrolling) {
			double t = UIE_Time();
			Rml::Vector2f delta = g->last_pos - now;
			g->pos += delta;
			ApplyScroll(g->scroll_target);
			double dt = t - g->last_move_time;
			if (dt > 0.0005) {
				Rml::Vector2f v = delta * (float)(1.0 / dt);
				g->velocity = g->velocity * 0.6f + v * 0.4f;
			}
			g->last_move_time = t;
			g->last_pos = now;
			return;
		}
	}
	g->last_pos = Rml::Vector2f(x, y);
	g->context->ProcessMouseMove((int)std::lround(x), (int)std::lround(y), 0);
	if (g->touch_mode)
		ClearHover();
}

int UIRml_MouseButton(int button, int down, int modifiers, int touch)
{
	if (!g)
		return 0;
	if (button == 0) {
		if (down) {
			g->pressed = true;
			g->touch = touch != 0;
			g->scrolling = false;
			g->left_target = false;
			// a touch on a list that is still flying or springing back stops
			// it, and is not a click on what is under the finger
			Rml::Vector2f v = g->velocity;
			g->swallow = g->touch && g->scroll_target && (std::fabs(v.x) > 20.0f || std::fabs(v.y) > 20.0f || g->shown_offset.x != 0 || g->shown_offset.y != 0);
			if (!g->swallow)
				g->scroll_target = nullptr;
			g->velocity = Rml::Vector2f(0, 0);
			g->last_move_time = UIE_Time();
			g->press_pos = g->last_pos; // UIRml_MouseMove went first
			if (g->swallow)
				return 1;
		} else {
			bool was_scrolling = g->scrolling;
			g->pressed = false;
			g->scrolling = false;
			if (g->swallow && !was_scrolling) {
				g->swallow = false;
				return 1; // the tap that stopped the list ends here
			}
			if (was_scrolling) {
				if (g->scroll_target) {
					char line[96];
					snprintf(line, sizeof(line), "dragged a list to scroll position %d", (int)g->scroll_target->GetScrollTop());
					UIE_Log(2, line);
				}
				// the context was told the pointer left; release without a click
				g->context->ProcessMouseButtonUp(0, modifiers);
				SetPressed(false);
				g->press_target = nullptr;
				return 1;
			}
		}
	}
	bool r = down ? g->context->ProcessMouseButtonDown(button, modifiers) : g->context->ProcessMouseButtonUp(button, modifiers);
	(void)r;
	if (button == 0 && g->touch) {
		if (down) {
			g->press_target = g->context->GetHoverElement();
			ClearHover();
		} else {
			g->context->ProcessMouseLeave(); // a lifted finger leaves nothing hovered
			SetPressed(false);
			g->press_target = nullptr;
		}
	}
	return 1;
}

int UIRml_MouseWheel(float delta, int modifiers)
{
	if (!g)
		return 0;
	// the engine's wheel up is RmlUi's negative
	g->context->ProcessMouseWheel(-delta, modifiers);
	return 1;
}

static int ModifierBits(int m)
{
	int r = 0;
	if (m & UIMOD_CTRL)
		r |= Rml::Input::KM_CTRL;
	if (m & UIMOD_SHIFT)
		r |= Rml::Input::KM_SHIFT;
	if (m & UIMOD_ALT)
		r |= Rml::Input::KM_ALT;
	return r;
}

static Rml::Input::KeyIdentifier TranslateKey(int key)
{
	using namespace Rml::Input;
	if (key >= 'a' && key <= 'z')
		return (KeyIdentifier)(KI_A + (key - 'a'));
	if (key >= '0' && key <= '9')
		return (KeyIdentifier)(KI_0 + (key - '0'));
	switch (key) {
	case ' ': return KI_SPACE;
	case UIKEY_ESCAPE: return KI_ESCAPE;
	case UIKEY_TAB: return KI_TAB;
	case UIKEY_RETURN: return KI_RETURN;
	case UIKEY_BACKSPACE: return KI_BACK;
	case UIKEY_DELETE: return KI_DELETE;
	case UIKEY_INSERT: return KI_INSERT;
	case UIKEY_HOME: return KI_HOME;
	case UIKEY_END: return KI_END;
	case UIKEY_PAGEUP: return KI_PRIOR;
	case UIKEY_PAGEDOWN: return KI_NEXT;
	case UIKEY_LEFT: return KI_LEFT;
	case UIKEY_RIGHT: return KI_RIGHT;
	case UIKEY_UP: return KI_UP;
	case UIKEY_DOWN: return KI_DOWN;
	default: return KI_UNKNOWN;
	}
}

int UIRml_Key(int key, int down, int modifiers)
{
	if (!g)
		return 0;
	Rml::Input::KeyIdentifier k = TranslateKey(key);
	if (k == Rml::Input::KI_UNKNOWN)
		return 0;
	int m = ModifierBits(modifiers);
	// RmlUi returns true when nothing used the key
	bool unused = down ? g->context->ProcessKeyDown(k, m) : g->context->ProcessKeyUp(k, m);
	return unused ? 0 : 1;
}

int UIRml_Text(unsigned int unicode)
{
	if (!g)
		return 0;
	g->context->ProcessTextInput((Rml::Character)unicode);
	return 1;
}

void UIRml_MouseLeave(void)
{
	if (g)
		g->context->ProcessMouseLeave();
}

int UIRml_TextFocus(void)
{
	if (!g)
		return 0;
	Rml::Element* f = g->context->GetFocusElement();
	if (!f)
		return 0;
	const Rml::String& tag = f->GetTagName();
	return tag == "input" || tag == "textarea";
}

int UIRml_Dragging(void)
{
	return g && g->scrolling;
}

void UIRml_Frame(void)
{
	if (!g)
		return;
	double now = UIE_Time();
	double dt = now - g->last_frame_time;
	g->last_frame_time = now;
	// closing from inside a click would pull the document from under its own event
	std::vector<std::string> closing;
	closing.swap(g->pending_close);
	for (const std::string& n : closing)
		CloseDocument(n);
	// a page from a server does not stay up for ever
	double limit = atof(UIE_CvarString("cl_server_page_timeout"));
	if (limit > 0)
		for (auto& kv : g->documents)
			if (kv.second.server && kv.second.shown && now - kv.second.shown_at > limit) {
				CloseDocument(kv.first);
				break;
			}
	// The fling after a drag: keep scrolling and slow down by friction. Past
	// an end a spring pulls the content back instead, without overshooting.
	if (g->scroll_target && !g->pressed) {
		if (dt > 0.05)
			dt = 0.05;
		Rml::Element* e = g->scroll_target;
		Rml::Vector2f max = ScrollMax(e);
		float* p[2] = {&g->pos.x, &g->pos.y};
		float* v[2] = {&g->velocity.x, &g->velocity.y};
		float hi[2] = {max.x, max.y};
		bool settled = true;
		for (int i = 0; i < 2; i++) {
			float clamped = std::min(std::max(*p[i], 0.0f), hi[i]);
			float excess = *p[i] - clamped;
			if (excess != 0.0f) {
				*v[i] += (-120.0f * excess - 22.0f * *v[i]) * (float)dt;
			} else {
				*v[i] *= (float)std::pow(0.04, dt); // about 0.3 s to a stop
			}
			*p[i] += *v[i] * (float)dt;
			float now_excess = *p[i] - std::min(std::max(*p[i], 0.0f), hi[i]);
			if (std::fabs(*v[i]) > 20.0f || std::fabs(now_excess) > 0.5f)
				settled = false;
		}
		if (settled) {
			g->velocity = Rml::Vector2f(0, 0);
			g->pos.x = std::min(std::max(g->pos.x, 0.0f), max.x);
			g->pos.y = std::min(std::max(g->pos.y, 0.0f), max.y);
			ApplyScroll(e);
			EndScroll();
		} else {
			ApplyScroll(e);
		}
	}
	UpdateBindings();
	auto shown = [](const char* n) {
		auto it = g->documents.find(n);
		return it != g->documents.end() && it->second.shown;
	};
	UIRmlModels_Update(now, shown("servers"), shown("singleplayer"), shown("settings"));
	g->context->Update();
	g->context->Render();
}

} // extern "C"
