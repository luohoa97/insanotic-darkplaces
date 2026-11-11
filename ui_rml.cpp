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
			level = 0;
			break;
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

// data-cmd on an element: queue that console command when it is clicked
class CommandListener : public Rml::EventListener {
public:
	void ProcessEvent(Rml::Event& event) override
	{
		// the click may land on a child of the element that carries the attribute
		for (Rml::Element* e = event.GetCurrentElement(); e; e = e->GetParentNode()) {
			if (e->HasAttribute("data-cmd")) {
				Rml::String cmd = e->GetAttribute<Rml::String>("data-cmd", "");
				if (!cmd.empty())
					UIE_Command(cmd.c_str());
				return;
			}
		}
	}
};

struct Document {
	Rml::ElementDocument* doc = nullptr;
	bool shown = false;
	std::map<std::string, bool> classes;
};

struct State {
	EngineFiles files;
	EngineSystem system;
	EngineRender render;
	CommandListener commands;
	Rml::Context* context = nullptr;
	bool initialised = false;
	std::map<std::string, Document> documents;
	std::unordered_map<Rml::Element*, std::string> bound; // data-cvar elements and the text they show
	double next_binding = 0;

	// drag scrolling
	bool pressed = false;
	bool touch = false;
	bool scrolling = false;
	Rml::Vector2f press_pos, last_pos;
	Rml::Element* scroll_target = nullptr;
	Rml::Vector2f velocity; // pixels per second, for the fling after a drag
	double last_move_time = 0;
	double last_frame_time = 0;
	float dpratio = 1.0f;
};

State* g = nullptr;

std::string DocumentPath(const std::string& name) { return "ui/" + name + ".rml"; }

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

void ScrollBy(Rml::Element* e, Rml::Vector2f delta)
{
	if (!e)
		return;
	e->SetScrollTop(e->GetScrollTop() + delta.y);
	e->SetScrollLeft(e->GetScrollLeft() + delta.x);
}

void PrepareDocument(Document& d)
{
	Rml::ElementList list;
	d.doc->QuerySelectorAll(list, "[data-cmd]");
	for (Rml::Element* e : list)
		e->AddEventListener("click", &g->commands);
	for (const auto& c : d.classes)
		if (Rml::Element* body = d.doc)
			body->SetClass(c.first, c.second);
}

// shows the cvar named in data-cvar as the element's text; checked a few
// times a second, not every frame
void UpdateBindings()
{
	double now = UIE_Time();
	if (now < g->next_binding)
		return;
	g->next_binding = now + 0.25;
	for (auto& kv : g->documents) {
		if (!kv.second.shown)
			continue;
		Rml::ElementList list;
		kv.second.doc->QuerySelectorAll(list, "[data-cvar]");
		for (Rml::Element* e : list) {
			Rml::String value = UIE_CvarString(e->GetAttribute<Rml::String>("data-cvar", "").c_str());
			auto it = g->bound.find(e);
			if (it != g->bound.end() && it->second == value)
				continue;
			g->bound[e] = value;
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
	it->second.doc->Hide();
	it->second.shown = false;
	g->pressed = false;
	g->scrolling = false;
}

void UIRml_HideAll(void)
{
	if (!g)
		return;
	for (auto& kv : g->documents)
		if (kv.second.shown) {
			kv.second.doc->Hide();
			kv.second.shown = false;
		}
	g->pressed = false;
	g->scrolling = false;
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

void UIRml_EachShown(void (*each)(const char* name))
{
	if (!g)
		return;
	for (auto& kv : g->documents)
		if (kv.second.shown)
			each(kv.first.c_str());
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
				g->scroll_target = FindScrollable(g->context->GetHoverElement());
				if (g->scroll_target) {
					g->scrolling = true;
					// take the press back: the element under the finger must not click
					g->context->ProcessMouseLeave();
					g->last_pos = g->press_pos;
				}
			}
		}
		if (g->scrolling) {
			double t = UIE_Time();
			Rml::Vector2f delta = g->last_pos - now;
			ScrollBy(g->scroll_target, delta);
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
			g->scroll_target = nullptr;
			g->velocity = Rml::Vector2f(0, 0);
			g->last_move_time = UIE_Time();
			g->press_pos = g->last_pos; // UIRml_MouseMove went first
		} else {
			bool was_scrolling = g->scrolling;
			g->pressed = false;
			g->scrolling = false;
			if (was_scrolling) {
				if (g->scroll_target) {
					char line[96];
					snprintf(line, sizeof(line), "dragged a list to scroll position %d", (int)g->scroll_target->GetScrollTop());
					UIE_Log(2, line);
				}
				// the context was told the pointer left; release without a click
				g->context->ProcessMouseButtonUp(0, modifiers);
				return 1;
			}
		}
	}
	bool r = down ? g->context->ProcessMouseButtonDown(button, modifiers) : g->context->ProcessMouseButtonUp(button, modifiers);
	(void)r;
	if (!down && g->touch && button == 0)
		g->context->ProcessMouseLeave(); // a lifted finger leaves nothing hovered
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
	// the fling after a drag: keep scrolling, slow down by friction
	if (g->scroll_target && !g->pressed && (std::fabs(g->velocity.x) > 20.0f || std::fabs(g->velocity.y) > 20.0f)) {
		ScrollBy(g->scroll_target, g->velocity * (float)dt);
		g->velocity = g->velocity * (float)std::pow(0.04, dt); // about 0.3 s to a stop
	} else if (!g->pressed) {
		g->scroll_target = nullptr;
	}
	UpdateBindings();
	g->context->Update();
	g->context->Render();
}

} // extern "C"
