#include "../../util/util.h"
#include "../browser.h"
#include "../helpline.h"
#include "../libslang.h"
#include "../ui.h"
#include "../util.h"
#include "../../util/annotate.h"
#include "../../util/hist.h"
#include "../../util/sort.h"
#include "../../util/symbol.h"
#include <pthread.h>
#include <newt.h>

struct browser_disasm_line {
	struct rb_node	rb_node;
	double		percent;
	u32		idx;
	int		idx_asm;
	bool		jump_target;
};

struct annotate_browser {
	struct ui_browser b;
	struct rb_root	  entries;
	struct rb_node	  *curr_hot;
	struct disasm_line	  *selection;
	struct disasm_line  **offsets;
	u64		    start;
	int		    nr_asm_entries;
	int		    nr_entries;
	bool		    hide_src_code;
	bool		    use_offset;
	bool		    searching_backwards;
	u8		    offset_width;
	char		    search_bf[128];
};

static inline struct browser_disasm_line *disasm_line__browser(struct disasm_line *dl)
{
	return (struct browser_disasm_line *)(dl + 1);
}

static bool disasm_line__filter(struct ui_browser *browser, void *entry)
{
	struct annotate_browser *ab = container_of(browser, struct annotate_browser, b);

	if (ab->hide_src_code) {
		struct disasm_line *dl = list_entry(entry, struct disasm_line, node);
		return dl->offset == -1;
	}

	return false;
}

static void annotate_browser__write(struct ui_browser *self, void *entry, int row)
{
	struct annotate_browser *ab = container_of(self, struct annotate_browser, b);
	struct disasm_line *dl = list_entry(entry, struct disasm_line, node);
	struct browser_disasm_line *bdl = disasm_line__browser(dl);
	bool current_entry = ui_browser__is_current_entry(self, row);
	bool change_color = (!ab->hide_src_code &&
			     (!current_entry || (self->use_navkeypressed &&
					         !self->navkeypressed)));
	int width = self->width;

	if (dl->offset != -1) {
		ui_browser__set_percent_color(self, bdl->percent, current_entry);
		slsmg_printf(" %7.2f ", bdl->percent);
	} else {
		ui_browser__set_percent_color(self, 0, current_entry);
		slsmg_write_nstring(" ", 9);
	}

	SLsmg_write_char(':');
	slsmg_write_nstring(" ", 8);

	/* The scroll bar isn't being used */
	if (!self->navkeypressed)
		width += 1;

	if (dl->offset != -1 && change_color)
		ui_browser__set_color(self, HE_COLORSET_CODE);

	if (!*dl->line)
		slsmg_write_nstring(" ", width - 10);
	else if (dl->offset == -1)
		slsmg_write_nstring(dl->line, width - 10);
	else {
		char bf[256];
		u64 addr = dl->offset;
		int printed, color = -1;

		if (!ab->use_offset)
			addr += ab->start;

		if (!ab->use_offset) {
			printed = scnprintf(bf, sizeof(bf), "%" PRIx64 ":", addr);
		} else {
			if (bdl->jump_target) {
				printed = scnprintf(bf, sizeof(bf), "%*" PRIx64 ":",
						    ab->offset_width, addr);
			} else {
				printed = scnprintf(bf, sizeof(bf), "%*s ",
						    ab->offset_width, " ");
			}
		}

		if (change_color)
			color = ui_browser__set_color(self, HE_COLORSET_ADDR);
		slsmg_write_nstring(bf, printed);
		if (change_color)
			ui_browser__set_color(self, color);
		if (dl->ins && dl->ins->ops->scnprintf) {
			dl->ins->ops->scnprintf(dl->ins, bf, sizeof(bf),
						!ab->use_offset ? dl->operands : NULL,
						dl->target);
			slsmg_write_nstring(" ", 2);
			printed += 2;
		} else
			scnprintf(bf, sizeof(bf), "  %-6.6s %s", dl->name, dl->operands);

		slsmg_write_nstring(bf, width - 10 - printed);
	}

	if (current_entry)
		ab->selection = dl;
}

static double disasm_line__calc_percent(struct disasm_line *dl, struct symbol *sym, int evidx)
{
	double percent = 0.0;

	if (dl->offset != -1) {
		int len = sym->end - sym->start;
		unsigned int hits = 0;
		struct annotation *notes = symbol__annotation(sym);
		struct source_line *src_line = notes->src->lines;
		struct sym_hist *h = annotation__histogram(notes, evidx);
		s64 offset = dl->offset;
		struct disasm_line *next;

		next = disasm__get_next_ip_line(&notes->src->source, dl);
		while (offset < (s64)len &&
		       (next == NULL || offset < next->offset)) {
			if (src_line) {
				percent += src_line[offset].percent;
			} else
				hits += h->addr[offset];

			++offset;
		}
		/*
 		 * If the percentage wasn't already calculated in
 		 * symbol__get_source_line, do it now:
 		 */
		if (src_line == NULL && h->sum)
			percent = 100.0 * hits / h->sum;
	}

	return percent;
}

static void disasm_rb_tree__insert(struct rb_root *root, struct browser_disasm_line *bdl)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct browser_disasm_line *l;

	while (*p != NULL) {
		parent = *p;
		l = rb_entry(parent, struct browser_disasm_line, rb_node);
		if (bdl->percent < l->percent)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&bdl->rb_node, parent, p);
	rb_insert_color(&bdl->rb_node, root);
}

static void annotate_browser__set_top(struct annotate_browser *self,
				      struct disasm_line *pos, u32 idx)
{
	unsigned back;

	ui_browser__refresh_dimensions(&self->b);
	back = self->b.height / 2;
	self->b.top_idx = self->b.index = idx;

	while (self->b.top_idx != 0 && back != 0) {
		pos = list_entry(pos->node.prev, struct disasm_line, node);

		if (disasm_line__filter(&self->b, &pos->node))
			continue;

		--self->b.top_idx;
		--back;
	}

	self->b.top = pos;
	self->b.navkeypressed = true;
}

static void annotate_browser__set_rb_top(struct annotate_browser *browser,
					 struct rb_node *nd)
{
	struct browser_disasm_line *bpos;
	struct disasm_line *pos;

	bpos = rb_entry(nd, struct browser_disasm_line, rb_node);
	pos = ((struct disasm_line *)bpos) - 1;
	annotate_browser__set_top(browser, pos, bpos->idx);
	browser->curr_hot = nd;
}

static void annotate_browser__calc_percent(struct annotate_browser *browser,
					   int evidx)
{
	struct map_symbol *ms = browser->b.priv;
	struct symbol *sym = ms->sym;
	struct annotation *notes = symbol__annotation(sym);
	struct disasm_line *pos;

	browser->entries = RB_ROOT;

	pthread_mutex_lock(&notes->lock);

	list_for_each_entry(pos, &notes->src->source, node) {
		struct browser_disasm_line *bpos = disasm_line__browser(pos);
		bpos->percent = disasm_line__calc_percent(pos, sym, evidx);
		if (bpos->percent < 0.01) {
			RB_CLEAR_NODE(&bpos->rb_node);
			continue;
		}
		disasm_rb_tree__insert(&browser->entries, bpos);
	}
	pthread_mutex_unlock(&notes->lock);

	browser->curr_hot = rb_last(&browser->entries);
}

static bool annotate_browser__toggle_source(struct annotate_browser *browser)
{
	struct disasm_line *dl;
	struct browser_disasm_line *bdl;
	off_t offset = browser->b.index - browser->b.top_idx;

	browser->b.seek(&browser->b, offset, SEEK_CUR);
	dl = list_entry(browser->b.top, struct disasm_line, node);
	bdl = disasm_line__browser(dl);

	if (browser->hide_src_code) {
		if (bdl->idx_asm < offset)
			offset = bdl->idx;

		browser->b.nr_entries = browser->nr_entries;
		browser->hide_src_code = false;
		browser->b.seek(&browser->b, -offset, SEEK_CUR);
		browser->b.top_idx = bdl->idx - offset;
		browser->b.index = bdl->idx;
	} else {
		if (bdl->idx_asm < 0) {
			ui_helpline__puts("Only available for assembly lines.");
			browser->b.seek(&browser->b, -offset, SEEK_CUR);
			return false;
		}

		if (bdl->idx_asm < offset)
			offset = bdl->idx_asm;

		browser->b.nr_entries = browser->nr_asm_entries;
		browser->hide_src_code = true;
		browser->b.seek(&browser->b, -offset, SEEK_CUR);
		browser->b.top_idx = bdl->idx_asm - offset;
		browser->b.index = bdl->idx_asm;
	}

	return true;
}

static bool annotate_browser__callq(struct annotate_browser *browser,
				    int evidx, void (*timer)(void *arg),
				    void *arg, int delay_secs)
{
	struct map_symbol *ms = browser->b.priv;
	struct disasm_line *dl = browser->selection;
	struct symbol *sym = ms->sym;
	struct annotation *notes;
	struct symbol *target;
	u64 ip;

	if (!ins__is_call(dl->ins))
		return false;

	ip = ms->map->map_ip(ms->map, dl->target);
	target = map__find_symbol(ms->map, ip, NULL);
	if (target == NULL) {
		ui_helpline__puts("The called function was not found.");
		return true;
	}

	notes = symbol__annotation(target);
	pthread_mutex_lock(&notes->lock);

	if (notes->src == NULL && symbol__alloc_hist(target) < 0) {
		pthread_mutex_unlock(&notes->lock);
		ui__warning("Not enough memory for annotating '%s' symbol!\n",
			    target->name);
		return true;
	}

	pthread_mutex_unlock(&notes->lock);
	symbol__tui_annotate(target, ms->map, evidx, timer, arg, delay_secs);
	ui_browser__show_title(&browser->b, sym->name);
	return true;
}

static
struct disasm_line *annotate_browser__find_offset(struct annotate_browser *browser,
					  s64 offset, s64 *idx)
{
	struct map_symbol *ms = browser->b.priv;
	struct symbol *sym = ms->sym;
	struct annotation *notes = symbol__annotation(sym);
	struct disasm_line *pos;

	*idx = 0;
	list_for_each_entry(pos, &notes->src->source, node) {
		if (pos->offset == offset)
			return pos;
		if (!disasm_line__filter(&browser->b, &pos->node))
			++*idx;
	}

	return NULL;
}

static bool annotate_browser__jump(struct annotate_browser *browser)
{
	struct disasm_line *dl = browser->selection;
	s64 idx;

	if (!ins__is_jump(dl->ins))
		return false;

	dl = annotate_browser__find_offset(browser, dl->target, &idx);
	if (dl == NULL) {
		ui_helpline__puts("Invallid jump offset");
		return true;
	}

	annotate_browser__set_top(browser, dl, idx);
	
	return true;
}

static
struct disasm_line *annotate_browser__find_string(struct annotate_browser *browser,
					  char *s, s64 *idx)
{
	struct map_symbol *ms = browser->b.priv;
	struct symbol *sym = ms->sym;
	struct annotation *notes = symbol__annotation(sym);
	struct disasm_line *pos = browser->selection;

	*idx = browser->b.index;
	list_for_each_entry_continue(pos, &notes->src->source, node) {
		if (disasm_line__filter(&browser->b, &pos->node))
			continue;

		++*idx;

		if (pos->line && strstr(pos->line, s) != NULL)
			return pos;
	}

	return NULL;
}

static bool __annotate_browser__search(struct annotate_browser *browser)
{
	struct disasm_line *dl;
	s64 idx;

	dl = annotate_browser__find_string(browser, browser->search_bf, &idx);
	if (dl == NULL) {
		ui_helpline__puts("String not found!");
		return false;
	}

	annotate_browser__set_top(browser, dl, idx);
	browser->searching_backwards = false;
	return true;
}

static
struct disasm_line *annotate_browser__find_string_reverse(struct annotate_browser *browser,
						  char *s, s64 *idx)
{
	struct map_symbol *ms = browser->b.priv;
	struct symbol *sym = ms->sym;
	struct annotation *notes = symbol__annotation(sym);
	struct disasm_line *pos = browser->selection;

	*idx = browser->b.index;
	list_for_each_entry_continue_reverse(pos, &notes->src->source, node) {
		if (disasm_line__filter(&browser->b, &pos->node))
			continue;

		--*idx;

		if (pos->line && strstr(pos->line, s) != NULL)
			return pos;
	}

	return NULL;
}

static bool __annotate_browser__search_reverse(struct annotate_browser *browser)
{
	struct disasm_line *dl;
	s64 idx;

	dl = annotate_browser__find_string_reverse(browser, browser->search_bf, &idx);
	if (dl == NULL) {
		ui_helpline__puts("String not found!");
		return false;
	}

	annotate_browser__set_top(browser, dl, idx);
	browser->searching_backwards = true;
	return true;
}

static bool annotate_browser__search_window(struct annotate_browser *browser,
					    int delay_secs)
{
	if (ui_browser__input_window("Search", "String: ", browser->search_bf,
				     "ENTER: OK, ESC: Cancel",
				     delay_secs * 2) != K_ENTER ||
	    !*browser->search_bf)
		return false;

	return true;
}

static bool annotate_browser__search(struct annotate_browser *browser, int delay_secs)
{
	if (annotate_browser__search_window(browser, delay_secs))
		return __annotate_browser__search(browser);

	return false;
}

static bool annotate_browser__continue_search(struct annotate_browser *browser,
					      int delay_secs)
{
	if (!*browser->search_bf)
		return annotate_browser__search(browser, delay_secs);

	return __annotate_browser__search(browser);
}

static bool annotate_browser__search_reverse(struct annotate_browser *browser,
					   int delay_secs)
{
	if (annotate_browser__search_window(browser, delay_secs))
		return __annotate_browser__search_reverse(browser);

	return false;
}

static
bool annotate_browser__continue_search_reverse(struct annotate_browser *browser,
					       int delay_secs)
{
	if (!*browser->search_bf)
		return annotate_browser__search_reverse(browser, delay_secs);

	return __annotate_browser__search_reverse(browser);
}

static int annotate_browser__run(struct annotate_browser *self, int evidx,
				 void(*timer)(void *arg),
				 void *arg, int delay_secs)
{
	struct rb_node *nd = NULL;
	struct map_symbol *ms = self->b.priv;
	struct symbol *sym = ms->sym;
	const char *help = "<-/ESC: Exit, TAB/shift+TAB: Cycle hot lines, "
			   "H: Go to hottest line, ->/ENTER: Line action, "
			   "O: Toggle offset view, "
			   "S: Toggle source code view";
	int key;

	if (ui_browser__show(&self->b, sym->name, help) < 0)
		return -1;

	annotate_browser__calc_percent(self, evidx);

	if (self->curr_hot) {
		annotate_browser__set_rb_top(self, self->curr_hot);
		self->b.navkeypressed = false;
	}

	nd = self->curr_hot;

	while (1) {
		key = ui_browser__run(&self->b, delay_secs);

		if (delay_secs != 0) {
			annotate_browser__calc_percent(self, evidx);
			/*
			 * Current line focus got out of the list of most active
			 * lines, NULL it so that if TAB|UNTAB is pressed, we
			 * move to curr_hot (current hottest line).
			 */
			if (nd != NULL && RB_EMPTY_NODE(nd))
				nd = NULL;
		}

		switch (key) {
		case K_TIMER:
			if (timer != NULL)
				timer(arg);

			if (delay_secs != 0)
				symbol__annotate_decay_histogram(sym, evidx);
			continue;
		case K_TAB:
			if (nd != NULL) {
				nd = rb_prev(nd);
				if (nd == NULL)
					nd = rb_last(&self->entries);
			} else
				nd = self->curr_hot;
			break;
		case K_UNTAB:
			if (nd != NULL)
				nd = rb_next(nd);
				if (nd == NULL)
					nd = rb_first(&self->entries);
			else
				nd = self->curr_hot;
			break;
		case 'H':
		case 'h':
			nd = self->curr_hot;
			break;
		case 'S':
		case 's':
			if (annotate_browser__toggle_source(self))
				ui_helpline__puts(help);
			continue;
		case 'O':
		case 'o':
			self->use_offset = !self->use_offset;
			continue;
		case '/':
			if (annotate_browser__search(self, delay_secs)) {
show_help:
				ui_helpline__puts(help);
			}
			continue;
		case 'n':
			if (self->searching_backwards ?
			    annotate_browser__continue_search_reverse(self, delay_secs) :
			    annotate_browser__continue_search(self, delay_secs))
				goto show_help;
			continue;
		case '?':
			if (annotate_browser__search_reverse(self, delay_secs))
				goto show_help;
			continue;
		case K_ENTER:
		case K_RIGHT:
			if (self->selection == NULL)
				ui_helpline__puts("Huh? No selection. Report to linux-kernel@vger.kernel.org");
			else if (self->selection->offset == -1)
				ui_helpline__puts("Actions are only available for assembly lines.");
			else if (!self->selection->ins ||
				 !(annotate_browser__jump(self) ||
				   annotate_browser__callq(self, evidx, timer, arg, delay_secs)))
				ui_helpline__puts("Actions are only available for the 'callq' and jump instructions.");
			continue;
		case K_LEFT:
		case K_ESC:
		case 'q':
		case CTRL('c'):
			goto out;
		default:
			continue;
		}

		if (nd != NULL)
			annotate_browser__set_rb_top(self, nd);
	}
out:
	ui_browser__hide(&self->b);
	return key;
}

int hist_entry__tui_annotate(struct hist_entry *he, int evidx,
			     void(*timer)(void *arg), void *arg, int delay_secs)
{
	return symbol__tui_annotate(he->ms.sym, he->ms.map, evidx,
				    timer, arg, delay_secs);
}

static void annotate_browser__mark_jump_targets(struct annotate_browser *browser,
						size_t size)
{
	u64 offset;

	for (offset = 0; offset < size; ++offset) {
		struct disasm_line *dl = browser->offsets[offset], *dlt;
		struct browser_disasm_line *bdlt;

		if (!dl || !dl->ins || !ins__is_jump(dl->ins))
			continue;

		if (dl->target >= size) {
			ui__error("jump to after symbol!\n"
				  "size: %zx, jump target: %" PRIx64,
				  size, dl->target);
			continue;
		}

		dlt = browser->offsets[dl->target];
		bdlt = disasm_line__browser(dlt);
		bdlt->jump_target = true;
	}
		
}

int symbol__tui_annotate(struct symbol *sym, struct map *map, int evidx,
			 void(*timer)(void *arg), void *arg,
			 int delay_secs)
{
	struct disasm_line *pos, *n;
	struct annotation *notes;
	const size_t size = symbol__size(sym);
	struct map_symbol ms = {
		.map = map,
		.sym = sym,
	};
	struct annotate_browser browser = {
		.b = {
			.refresh = ui_browser__list_head_refresh,
			.seek	 = ui_browser__list_head_seek,
			.write	 = annotate_browser__write,
			.filter  = disasm_line__filter,
			.priv	 = &ms,
			.use_navkeypressed = true,
		},
		.use_offset = true,
	};
	int ret = -1;

	if (sym == NULL)
		return -1;

	if (map->dso->annotate_warned)
		return -1;

	browser.offsets = zalloc(size * sizeof(struct disasm_line *));
	if (browser.offsets == NULL) {
		ui__error("Not enough memory!");
		return -1;
	}

	if (symbol__annotate(sym, map, sizeof(struct browser_disasm_line)) < 0) {
		ui__error("%s", ui_helpline__last_msg);
		goto out_free_offsets;
	}

	ui_helpline__push("Press <- or ESC to exit");

	notes = symbol__annotation(sym);
	browser.start = map__rip_2objdump(map, sym->start);

	list_for_each_entry(pos, &notes->src->source, node) {
		struct browser_disasm_line *bpos;
		size_t line_len = strlen(pos->line);

		if (browser.b.width < line_len)
			browser.b.width = line_len;
		bpos = disasm_line__browser(pos);
		bpos->idx = browser.nr_entries++;
		if (pos->offset != -1) {
			bpos->idx_asm = browser.nr_asm_entries++;
			browser.offsets[pos->offset] = pos;
		} else
			bpos->idx_asm = -1;
	}

	annotate_browser__mark_jump_targets(&browser, size);

	browser.offset_width = hex_width(size);
	browser.b.nr_entries = browser.nr_entries;
	browser.b.entries = &notes->src->source,
	browser.b.width += 18; /* Percentage */
	ret = annotate_browser__run(&browser, evidx, timer, arg, delay_secs);
	list_for_each_entry_safe(pos, n, &notes->src->source, node) {
		list_del(&pos->node);
		disasm_line__free(pos);
	}

out_free_offsets:
	free(browser.offsets);
	return ret;
}
