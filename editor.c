#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "editor.h"
#include "util.h"

static EditorWin *editor_window_new_text(Editor *ed, Text *text);
static void editor_window_free(EditorWin *win);
static void editor_windows_invalidate(Editor *ed, size_t start, size_t end);

void editor_windows_arrange(Editor *ed, enum UiLayout layout) {
	ed->ui->arrange(ed->ui, layout);
}

bool editor_window_reload(EditorWin *win) {
	const char *filename = text_filename_get(win->text);
	/* can't reload unsaved file */
	if (!filename)
		return false;
	Text *text = text_load(filename);
	if (!text)
		return false;
	/* check wether the text is displayed in another window */
	bool needed = false;
	for (EditorWin *w = win->editor->windows; w; w = w->next) {
		if (w != win && w->text == win->text) {
			needed = true;
			break;
		}
	}
	if (!needed)
		text_free(win->text);
	win->text = text;
	window_reload(win->win, text);
	return true;
}

bool editor_window_split(EditorWin *original) {
	EditorWin *win = editor_window_new_text(original->editor, original->text);
	if (!win)
		return false;
	win->text = original->text;
	window_syntax_set(win->win, window_syntax_get(original->win));
	window_cursor_to(win->win, window_cursor_get(original->win));
	editor_draw(win->editor);
	return true;
}

void editor_window_jumplist_add(EditorWin *win, size_t pos) {
	Mark mark = text_mark_set(win->text, pos);
	if (mark && win->jumplist)
		ringbuf_add(win->jumplist, mark);
}

size_t editor_window_jumplist_prev(EditorWin *win) {
	size_t cur = window_cursor_get(win->win);
	while (win->jumplist) {
		Mark mark = ringbuf_prev(win->jumplist);
		if (!mark)
			return cur;
		size_t pos = text_mark_get(win->text, mark);
		if (pos != EPOS && pos != cur)
			return pos;
	}
	return cur;
}

size_t editor_window_jumplist_next(EditorWin *win) {
	size_t cur = window_cursor_get(win->win);
	while (win->jumplist) {
		Mark mark = ringbuf_next(win->jumplist);
		if (!mark)
			return cur;
		size_t pos = text_mark_get(win->text, mark);
		if (pos != EPOS && pos != cur)
			return pos;
	}
	return cur;
}

void editor_window_jumplist_invalidate(EditorWin *win) {
	if (win->jumplist)
		ringbuf_invalidate(win->jumplist);
}

size_t editor_window_changelist_prev(EditorWin *win) {
	size_t pos = window_cursor_get(win->win);
	if (pos != win->changelist.pos)
		win->changelist.index = 0;
	else
		win->changelist.index++;
	size_t newpos = text_history_get(win->text, win->changelist.index);
	if (newpos == EPOS)
		win->changelist.index--;
	else
		win->changelist.pos = newpos;
	return win->changelist.pos;
}

size_t editor_window_changelist_next(EditorWin *win) {
	size_t pos = window_cursor_get(win->win);
	if (pos != win->changelist.pos)
		win->changelist.index = 0;
	else if (win->changelist.index > 0)
		win->changelist.index--;
	size_t newpos = text_history_get(win->text, win->changelist.index);
	if (newpos == EPOS)
		win->changelist.index++;
	else
		win->changelist.pos = newpos;
	return win->changelist.pos;
}

void editor_resize(Editor *ed) {
	ed->ui->resize(ed->ui);
}

void editor_window_next(Editor *ed) {
	EditorWin *sel = ed->win;
	if (!sel)
		return;
	ed->win = ed->win->next;
	if (!ed->win)
		ed->win = ed->windows;
	ed->ui->window_focus(ed->win->ui);
}

void editor_window_prev(Editor *ed) {
	EditorWin *sel = ed->win;
	if (!sel)
		return;
	ed->win = ed->win->prev;
	if (!ed->win)
		for (ed->win = ed->windows; ed->win->next; ed->win = ed->win->next);
	ed->ui->window_focus(ed->win->ui);
}

static void editor_windows_invalidate(Editor *ed, size_t start, size_t end) {
	for (EditorWin *win = ed->windows; win; win = win->next) {
		if (ed->win != win && ed->win->text == win->text) {
			Filerange view = window_viewport_get(win->win);
			if ((view.start <= start && start <= view.end) ||
			    (view.start <= end && end <= view.end))
				win->ui->draw(win->ui);
		}
	}
	ed->win->ui->draw(ed->win->ui);
}

int editor_tabwidth_get(Editor *ed) {
	return ed->tabwidth;
}

void editor_tabwidth_set(Editor *ed, int tabwidth) {
	if (tabwidth < 1 || tabwidth > 8)
		return;
	for (EditorWin *win = ed->windows; win; win = win->next)
		window_tabwidth_set(win->win, tabwidth);
	ed->tabwidth = tabwidth;
}

bool editor_syntax_load(Editor *ed, Syntax *syntaxes, Color *colors) {
	bool success = true;
	ed->syntaxes = syntaxes;

	for (Color *color = colors; color && color->fg; color++) {
		if (color->attr == 0)
			color->attr = A_NORMAL;
		color->attr |= COLOR_PAIR(ed->ui->color_get(color->fg, color->bg));
	}

	for (Syntax *syn = syntaxes; syn && syn->name; syn++) {
		if (regcomp(&syn->file_regex, syn->file, REG_EXTENDED|REG_NOSUB|REG_ICASE|REG_NEWLINE))
			success = false;
		for (int j = 0; j < LENGTH(syn->rules); j++) {
			SyntaxRule *rule = &syn->rules[j];
			if (!rule->rule)
				break;
			int cflags = REG_EXTENDED;
			if (!rule->multiline)
				cflags |= REG_NEWLINE;
			if (regcomp(&rule->regex, rule->rule, cflags))
				success = false;
		}
	}

	return success;
}

void editor_syntax_unload(Editor *ed) {
	for (Syntax *syn = ed->syntaxes; syn && syn->name; syn++) {
		regfree(&syn->file_regex);
		for (int j = 0; j < LENGTH(syn->rules); j++) {
			SyntaxRule *rule = &syn->rules[j];
			if (!rule->rule)
				break;
			regfree(&rule->regex);
		}
	}

	ed->syntaxes = NULL;
}

void editor_draw(Editor *ed) {
	ed->ui->draw(ed->ui);
}

void editor_update(Editor *ed) {
	ed->ui->update(ed->ui);
}

void editor_suspend(Editor *ed) {
	ed->ui->suspend(ed->ui);
}

static void editor_window_free(EditorWin *win) {
	if (!win)
		return;
	Editor *ed = win->editor;
	if (ed && ed->ui)
		ed->ui->window_free(win->ui);
	ringbuf_free(win->jumplist);
	bool needed = false;
	for (EditorWin *w = ed ? ed->windows : NULL; w; w = w->next) {
		if (w->text == win->text) {
			needed = true;
			break;
		}
	}
	if (!needed)
		text_free(win->text);
	free(win);
}

static EditorWin *editor_window_new_text(Editor *ed, Text *text) {
	EditorWin *win = calloc(1, sizeof(EditorWin));
	if (!win)
		return NULL;
	win->editor = ed;
	win->text = text;
	win->jumplist = ringbuf_alloc(31);
	win->ui = ed->ui->window_new(ed->ui, text);
	if (!win->jumplist || !win->ui) {
		editor_window_free(win);
		return NULL;
	}
	win->win = win->ui->view_get(win->ui);
	window_tabwidth_set(win->win, ed->tabwidth);
	if (ed->windows)
		ed->windows->prev = win;
	win->next = ed->windows;
	win->prev = NULL;
	ed->windows = win;
	ed->win = win;
	ed->ui->window_focus(win->ui);
	return win;
}

bool editor_window_new(Editor *ed, const char *filename) {
	Text *text = NULL;
	/* try to detect whether the same file is already open in another window
	 * TODO: do this based on inodes */
	EditorWin *original = NULL;
	if (filename) {
		for (EditorWin *win = ed->windows; win; win = win->next) {
			const char *f = text_filename_get(win->text);
			if (f && strcmp(f, filename) == 0) {
				original = win;
				break;
			}
		}
	}

	if (original)
		text = original->text;
	else
		text = text_load(filename && access(filename, F_OK) == 0 ? filename : NULL);
	if (!text)
		return false;

	EditorWin *win = editor_window_new_text(ed, text);
	if (!win) {
		if (!original)
			text_free(text);
		return false;
	}

	if (original) {
		window_syntax_set(win->win, window_syntax_get(original->win));
		window_cursor_to(win->win, window_cursor_get(original->win));
	} else if (filename) {
		text_filename_set(text, filename);
		for (Syntax *syn = ed->syntaxes; syn && syn->name; syn++) {
			if (!regexec(&syn->file_regex, filename, 0, NULL, 0)) {
				window_syntax_set(win->win, syn);
				break;
			}
		}
	}

	editor_draw(ed);

	return true;
}

bool editor_window_new_fd(Editor *ed, int fd) {
	Text *txt = text_load_fd(fd);
	if (!txt)
		return false;
	EditorWin *win = editor_window_new_text(ed, txt);
	if (!win)
		return false;
	editor_draw(ed);
	return true;
}

void editor_window_close(EditorWin *win) {
	Editor *ed = win->editor;
	if (win->prev)
		win->prev->next = win->next;
	if (win->next)
		win->next->prev = win->prev;
	if (ed->windows == win)
		ed->windows = win->next;
	if (ed->win == win)
		ed->win = win->next ? win->next : win->prev;
	if (ed->win)
		ed->ui->window_focus(ed->win->ui);
	editor_window_free(win);
	editor_draw(ed);
}

Editor *editor_new(Ui *ui) {
	if (!ui)
		return NULL;
	Editor *ed = calloc(1, sizeof(Editor));
	if (!ed)
		return NULL;
	ed->ui = ui;
	ed->ui->init(ed->ui, ed);
	ed->tabwidth = 8;
	ed->expandtab = false;
	if (!(ed->prompt = calloc(1, sizeof(EditorWin))))
		goto err;
	if (!(ed->prompt->text = text_load(NULL)))
		goto err;
	if (!(ed->prompt->ui = ed->ui->prompt_new(ed->ui, ed->prompt->text)))
		goto err;
	if (!(ed->search_pattern = text_regex_new()))
		goto err;
	ed->prompt->win = ed->prompt->ui->view_get(ed->prompt->ui);
	return ed;
err:
	editor_free(ed);
	return NULL;
}

void editor_free(Editor *ed) {
	if (!ed)
		return;
	while (ed->windows)
		editor_window_close(ed->windows);
	editor_window_free(ed->prompt);
	text_regex_free(ed->search_pattern);
	for (int i = 0; i < REG_LAST; i++)
		register_free(&ed->registers[i]);
	editor_syntax_unload(ed);
	ed->ui->free(ed->ui);
	free(ed);
}

void editor_insert_key(Editor *ed, const char *c, size_t len) {
	Win *win = ed->win->win;
	size_t start = window_cursor_get(win);
	window_insert_key(win, c, len);
	editor_windows_invalidate(ed, start, start + len);
}

void editor_replace_key(Editor *ed, const char *c, size_t len) {
	Win *win = ed->win->win;
	size_t start = window_cursor_get(win);
	window_replace_key(win, c, len);
	editor_windows_invalidate(ed, start, start + 6);
}

void editor_backspace_key(Editor *ed) {
	Win *win = ed->win->win;
	size_t end = window_cursor_get(win);
	size_t start = window_backspace_key(win);
	editor_windows_invalidate(ed, start, end);
}

void editor_delete_key(Editor *ed) {
	size_t start = window_delete_key(ed->win->win);
	editor_windows_invalidate(ed, start, start + 6);
}

void editor_insert(Editor *ed, size_t pos, const char *c, size_t len) {
	text_insert(ed->win->text, pos, c, len);
	editor_windows_invalidate(ed, pos, pos + len);
}

void editor_delete(Editor *ed, size_t pos, size_t len) {
	text_delete(ed->win->text, pos, len);
	editor_windows_invalidate(ed, pos, pos + len);
}

void editor_prompt_show(Editor *ed, const char *title, const char *text) {
	if (ed->prompt_window)
		return;
	ed->prompt_window = ed->win;
	ed->win = ed->prompt;
	ed->prompt_type = title[0];
	ed->ui->prompt(ed->ui, title, text);
}

void editor_prompt_hide(Editor *ed) {
	if (!ed->prompt_window)
		return;
	ed->ui->prompt_hide(ed->ui);
	ed->win = ed->prompt_window;
	ed->prompt_window = NULL;
}

char *editor_prompt_get(Editor *ed) {
	return ed->ui->prompt_input(ed->ui);
}

void editor_info_show(Editor *ed, const char *msg, ...) {
	va_list ap;
	va_start(ap, msg);
	ed->ui->info(ed->ui, msg, ap);
	va_end(ap);
}

void editor_info_hide(Editor *ed) {
	ed->ui->info_hide(ed->ui);
}

void editor_window_options(EditorWin *win, enum UiOption options) {
	win->ui->options(win->ui, options);
}

