/*
 * trace_events_trigger - trace event triggers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) 2013 Tom Zanussi <tom.zanussi@linux.intel.com>
 */

#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/stacktrace.h>
#include <linux/sort.h>
#include <linux/bootmem.h>
#include <asm/setup.h>

#include "trace.h"

static LIST_HEAD(trigger_commands);
static DEFINE_MUTEX(trigger_cmd_mutex);

static void
trigger_data_free(struct event_trigger_data *data)
{
	if (data->cmd_ops->set_filter)
		data->cmd_ops->set_filter(NULL, data, NULL);

	synchronize_sched(); /* make sure current triggers exit before free */
	kfree(data);
}

/**
 * event_triggers_call - Call triggers associated with a trace event
 * @file: The ftrace_event_file associated with the event
 * @rec: The trace entry for the event, NULL for unconditional invocation
 *
 * For each trigger associated with an event, invoke the trigger
 * function registered with the associated trigger command.  If rec is
 * non-NULL, it means that the trigger requires further processing and
 * shouldn't be unconditionally invoked.  If rec is non-NULL and the
 * trigger has a filter associated with it, rec will checked against
 * the filter and if the record matches the trigger will be invoked.
 * If the trigger is a 'post_trigger', meaning it shouldn't be invoked
 * in any case until the current event is written, the trigger
 * function isn't invoked but the bit associated with the deferred
 * trigger is set in the return value.
 *
 * Returns an enum event_trigger_type value containing a set bit for
 * any trigger that should be deferred, ETT_NONE if nothing to defer.
 *
 * Called from tracepoint handlers (with rcu_read_lock_sched() held).
 *
 * Return: an enum event_trigger_type value containing a set bit for
 * any trigger that should be deferred, ETT_NONE if nothing to defer.
 */
enum event_trigger_type
event_triggers_call(struct ftrace_event_file *file, void *rec)
{
	struct event_trigger_data *data;
	enum event_trigger_type tt = ETT_NONE;
	struct event_filter *filter;

	if (list_empty(&file->triggers))
		return tt;

	list_for_each_entry_rcu(data, &file->triggers, list) {
		if (!rec) {
			data->ops->func(data, rec);
			continue;
		}
		filter = rcu_dereference(data->filter);
		if (filter && !filter_match_preds(filter, rec))
			continue;
		if (data->cmd_ops->post_trigger) {
			tt |= data->cmd_ops->trigger_type;
			continue;
		}
		data->ops->func(data, rec);
	}
	return tt;
}
EXPORT_SYMBOL_GPL(event_triggers_call);

/**
 * event_triggers_post_call - Call 'post_triggers' for a trace event
 * @file: The ftrace_event_file associated with the event
 * @tt: enum event_trigger_type containing a set bit for each trigger to invoke
 *
 * For each trigger associated with an event, invoke the trigger
 * function registered with the associated trigger command, if the
 * corresponding bit is set in the tt enum passed into this function.
 * See @event_triggers_call for details on how those bits are set.
 *
 * Called from tracepoint handlers (with rcu_read_lock_sched() held).
 */
void
event_triggers_post_call(struct ftrace_event_file *file,
			 enum event_trigger_type tt,
			 void *rec)
{
	struct event_trigger_data *data;

	list_for_each_entry_rcu(data, &file->triggers, list) {
		if (data->cmd_ops->trigger_type & tt)
			data->ops->func(data, rec);
	}
}
EXPORT_SYMBOL_GPL(event_triggers_post_call);

#define SHOW_AVAILABLE_TRIGGERS	(void *)(1UL)

static void *trigger_next(struct seq_file *m, void *t, loff_t *pos)
{
	struct ftrace_event_file *event_file = event_file_data(m->private);

	if (t == SHOW_AVAILABLE_TRIGGERS)
		return NULL;

	return seq_list_next(t, &event_file->triggers, pos);
}

static void *trigger_start(struct seq_file *m, loff_t *pos)
{
	struct ftrace_event_file *event_file;

	/* ->stop() is called even if ->start() fails */
	mutex_lock(&event_mutex);
	event_file = event_file_data(m->private);
	if (unlikely(!event_file))
		return ERR_PTR(-ENODEV);

	if (list_empty(&event_file->triggers))
		return *pos == 0 ? SHOW_AVAILABLE_TRIGGERS : NULL;

	return seq_list_start(&event_file->triggers, *pos);
}

static void trigger_stop(struct seq_file *m, void *t)
{
	mutex_unlock(&event_mutex);
}

static int trigger_show(struct seq_file *m, void *v)
{
	struct event_trigger_data *data;
	struct event_command *p;

	if (v == SHOW_AVAILABLE_TRIGGERS) {
		seq_puts(m, "# Available triggers:\n");
		seq_putc(m, '#');
		mutex_lock(&trigger_cmd_mutex);
		list_for_each_entry_reverse(p, &trigger_commands, list)
			seq_printf(m, " %s", p->name);
		seq_putc(m, '\n');
		mutex_unlock(&trigger_cmd_mutex);
		return 0;
	}

	data = list_entry(v, struct event_trigger_data, list);
	data->ops->print(m, data->ops, data);

	return 0;
}

static const struct seq_operations event_triggers_seq_ops = {
	.start = trigger_start,
	.next = trigger_next,
	.stop = trigger_stop,
	.show = trigger_show,
};

static int event_trigger_regex_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&event_mutex);

	if (unlikely(!event_file_data(file))) {
		mutex_unlock(&event_mutex);
		return -ENODEV;
	}

	if (file->f_mode & FMODE_READ) {
		ret = seq_open(file, &event_triggers_seq_ops);
		if (!ret) {
			struct seq_file *m = file->private_data;
			m->private = file;
		}
	}

	mutex_unlock(&event_mutex);

	return ret;
}

static int trigger_process_regex(struct ftrace_event_file *file, char *buff)
{
	char *command, *next = buff;
	struct event_command *p;
	int ret = -EINVAL;

	command = strsep(&next, ": \t");
	command = (command[0] != '!') ? command : command + 1;

	mutex_lock(&trigger_cmd_mutex);
	list_for_each_entry(p, &trigger_commands, list) {
		if (strcmp(p->name, command) == 0) {
			ret = p->func(p, file, buff, command, next);
			goto out_unlock;
		}
	}
 out_unlock:
	mutex_unlock(&trigger_cmd_mutex);

	return ret;
}

static ssize_t event_trigger_regex_write(struct file *file,
					 const char __user *ubuf,
					 size_t cnt, loff_t *ppos)
{
	struct ftrace_event_file *event_file;
	ssize_t ret;
	char *buf;

	if (!cnt)
		return 0;

	if (cnt >= PAGE_SIZE)
		return -EINVAL;

	buf = (char *)__get_free_page(GFP_TEMPORARY);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, ubuf, cnt)) {
		free_page((unsigned long)buf);
		return -EFAULT;
	}
	buf[cnt] = '\0';
	strim(buf);

	mutex_lock(&event_mutex);
	event_file = event_file_data(file);
	if (unlikely(!event_file)) {
		mutex_unlock(&event_mutex);
		free_page((unsigned long)buf);
		return -ENODEV;
	}
	ret = trigger_process_regex(event_file, buf);
	mutex_unlock(&event_mutex);

	free_page((unsigned long)buf);
	if (ret < 0)
		goto out;

	*ppos += cnt;
	ret = cnt;
 out:
	return ret;
}

static int event_trigger_regex_release(struct inode *inode, struct file *file)
{
	mutex_lock(&event_mutex);

	if (file->f_mode & FMODE_READ)
		seq_release(inode, file);

	mutex_unlock(&event_mutex);

	return 0;
}

static ssize_t
event_trigger_write(struct file *filp, const char __user *ubuf,
		    size_t cnt, loff_t *ppos)
{
	return event_trigger_regex_write(filp, ubuf, cnt, ppos);
}

static int
event_trigger_open(struct inode *inode, struct file *filp)
{
	return event_trigger_regex_open(inode, filp);
}

static int
event_trigger_release(struct inode *inode, struct file *file)
{
	return event_trigger_regex_release(inode, file);
}

const struct file_operations event_trigger_fops = {
	.open = event_trigger_open,
	.read = seq_read,
	.write = event_trigger_write,
	.llseek = tracing_lseek,
	.release = event_trigger_release,
};

/*
 * Currently we only register event commands from __init, so mark this
 * __init too.
 */
static __init int register_event_command(struct event_command *cmd)
{
	struct event_command *p;
	int ret = 0;

	mutex_lock(&trigger_cmd_mutex);
	list_for_each_entry(p, &trigger_commands, list) {
		if (strcmp(cmd->name, p->name) == 0) {
			ret = -EBUSY;
			goto out_unlock;
		}
	}
	list_add(&cmd->list, &trigger_commands);
 out_unlock:
	mutex_unlock(&trigger_cmd_mutex);

	return ret;
}

/*
 * Currently we only unregister event commands from __init, so mark
 * this __init too.
 */
static __init int unregister_event_command(struct event_command *cmd)
{
	struct event_command *p, *n;
	int ret = -ENODEV;

	mutex_lock(&trigger_cmd_mutex);
	list_for_each_entry_safe(p, n, &trigger_commands, list) {
		if (strcmp(cmd->name, p->name) == 0) {
			ret = 0;
			list_del_init(&p->list);
			goto out_unlock;
		}
	}
 out_unlock:
	mutex_unlock(&trigger_cmd_mutex);

	return ret;
}

/**
 * event_trigger_print - Generic event_trigger_ops @print implementation
 * @name: The name of the event trigger
 * @m: The seq_file being printed to
 * @data: Trigger-specific data
 * @filter_str: filter_str to print, if present
 *
 * Common implementation for event triggers to print themselves.
 *
 * Usually wrapped by a function that simply sets the @name of the
 * trigger command and then invokes this.
 *
 * Return: 0 on success, errno otherwise
 */
static int
event_trigger_print(const char *name, struct seq_file *m,
		    void *data, char *filter_str)
{
	long count = (long)data;

	seq_printf(m, "%s", name);

	if (count == -1)
		seq_puts(m, ":unlimited");
	else
		seq_printf(m, ":count=%ld", count);

	if (filter_str)
		seq_printf(m, " if %s\n", filter_str);
	else
		seq_puts(m, "\n");

	return 0;
}

/**
 * event_trigger_init - Generic event_trigger_ops @init implementation
 * @ops: The trigger ops associated with the trigger
 * @data: Trigger-specific data
 *
 * Common implementation of event trigger initialization.
 *
 * Usually used directly as the @init method in event trigger
 * implementations.
 *
 * Return: 0 on success, errno otherwise
 */
static int
event_trigger_init(struct event_trigger_ops *ops,
		   struct event_trigger_data *data)
{
	data->ref++;
	return 0;
}

/**
 * event_trigger_free - Generic event_trigger_ops @free implementation
 * @ops: The trigger ops associated with the trigger
 * @data: Trigger-specific data
 *
 * Common implementation of event trigger de-initialization.
 *
 * Usually used directly as the @free method in event trigger
 * implementations.
 */
static void
event_trigger_free(struct event_trigger_ops *ops,
		   struct event_trigger_data *data)
{
	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref)
		trigger_data_free(data);
}

static int trace_event_trigger_enable_disable(struct ftrace_event_file *file,
					      int trigger_enable)
{
	int ret = 0;

	if (trigger_enable) {
		if (atomic_inc_return(&file->tm_ref) > 1)
			return ret;
		set_bit(FTRACE_EVENT_FL_TRIGGER_MODE_BIT, &file->flags);
		ret = trace_event_enable_disable(file, 1, 1);
	} else {
		if (atomic_dec_return(&file->tm_ref) > 0)
			return ret;
		clear_bit(FTRACE_EVENT_FL_TRIGGER_MODE_BIT, &file->flags);
		ret = trace_event_enable_disable(file, 0, 1);
	}

	return ret;
}

/**
 * clear_event_triggers - Clear all triggers associated with a trace array
 * @tr: The trace array to clear
 *
 * For each trigger, the triggering event has its tm_ref decremented
 * via trace_event_trigger_enable_disable(), and any associated event
 * (in the case of enable/disable_event triggers) will have its sm_ref
 * decremented via free()->trace_event_enable_disable().  That
 * combination effectively reverses the soft-mode/trigger state added
 * by trigger registration.
 *
 * Must be called with event_mutex held.
 */
void
clear_event_triggers(struct trace_array *tr)
{
	struct ftrace_event_file *file;

	list_for_each_entry(file, &tr->events, list) {
		struct event_trigger_data *data;
		list_for_each_entry_rcu(data, &file->triggers, list) {
			trace_event_trigger_enable_disable(file, 0);
			if (data->ops->free)
				data->ops->free(data->ops, data);
		}
	}
}

/**
 * update_cond_flag - Set or reset the TRIGGER_COND bit
 * @file: The ftrace_event_file associated with the event
 *
 * If an event has triggers and any of those triggers has a filter or
 * a post_trigger, trigger invocation needs to be deferred until after
 * the current event has logged its data, and the event should have
 * its TRIGGER_COND bit set, otherwise the TRIGGER_COND bit should be
 * cleared.
 */
static void update_cond_flag(struct ftrace_event_file *file)
{
	struct event_trigger_data *data;
	bool set_cond = false;

	list_for_each_entry_rcu(data, &file->triggers, list) {
		if (data->filter || data->cmd_ops->post_trigger) {
			set_cond = true;
			break;
		}
	}

	if (set_cond)
		set_bit(FTRACE_EVENT_FL_TRIGGER_COND_BIT, &file->flags);
	else
		clear_bit(FTRACE_EVENT_FL_TRIGGER_COND_BIT, &file->flags);
}

/**
 * register_trigger - Generic event_command @reg implementation
 * @glob: The raw string used to register the trigger
 * @ops: The trigger ops associated with the trigger
 * @data: Trigger-specific data to associate with the trigger
 * @file: The ftrace_event_file associated with the event
 *
 * Common implementation for event trigger registration.
 *
 * Usually used directly as the @reg method in event command
 * implementations.
 *
 * Return: 0 on success, errno otherwise
 */
static int register_trigger(char *glob, struct event_trigger_ops *ops,
			    struct event_trigger_data *data,
			    struct ftrace_event_file *file)
{
	struct event_trigger_data *test;
	int ret = 0;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == data->cmd_ops->trigger_type) {
			ret = -EEXIST;
			goto out;
		}
	}

	if (data->ops->init) {
		ret = data->ops->init(data->ops, data);
		if (ret < 0)
			goto out;
	}

	list_add_rcu(&data->list, &file->triggers);
	ret++;

	if (trace_event_trigger_enable_disable(file, 1) < 0) {
		list_del_rcu(&data->list);
		ret--;
	}
	update_cond_flag(file);
out:
	return ret;
}

/**
 * unregister_trigger - Generic event_command @unreg implementation
 * @glob: The raw string used to register the trigger
 * @ops: The trigger ops associated with the trigger
 * @test: Trigger-specific data used to find the trigger to remove
 * @file: The ftrace_event_file associated with the event
 *
 * Common implementation for event trigger unregistration.
 *
 * Usually used directly as the @unreg method in event command
 * implementations.
 */
static void unregister_trigger(char *glob, struct event_trigger_ops *ops,
			       struct event_trigger_data *test,
			       struct ftrace_event_file *file)
{
	struct event_trigger_data *data;
	bool unregistered = false;

	list_for_each_entry_rcu(data, &file->triggers, list) {
		if (data->cmd_ops->trigger_type == test->cmd_ops->trigger_type) {
			unregistered = true;
			list_del_rcu(&data->list);
			update_cond_flag(file);
			trace_event_trigger_enable_disable(file, 0);
			break;
		}
	}

	if (unregistered && data->ops->free)
		data->ops->free(data->ops, data);
}

/**
 * event_trigger_callback - Generic event_command @func implementation
 * @cmd_ops: The command ops, used for trigger registration
 * @file: The ftrace_event_file associated with the event
 * @glob: The raw string used to register the trigger
 * @cmd: The cmd portion of the string used to register the trigger
 * @param: The params portion of the string used to register the trigger
 *
 * Common implementation for event command parsing and trigger
 * instantiation.
 *
 * Usually used directly as the @func method in event command
 * implementations.
 *
 * Return: 0 on success, errno otherwise
 */
static int
event_trigger_callback(struct event_command *cmd_ops,
		       struct ftrace_event_file *file,
		       char *glob, char *cmd, char *param)
{
	struct event_trigger_data *trigger_data;
	struct event_trigger_ops *trigger_ops;
	char *trigger = NULL;
	char *number;
	int ret;

	/* separate the trigger from the filter (t:n [if filter]) */
	if (param && isdigit(param[0]))
		trigger = strsep(&param, " \t");

	trigger_ops = cmd_ops->get_trigger_ops(cmd, trigger);

	ret = -ENOMEM;
	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data)
		goto out;

	trigger_data->count = -1;
	trigger_data->ops = trigger_ops;
	trigger_data->cmd_ops = cmd_ops;
	INIT_LIST_HEAD(&trigger_data->list);

	if (glob[0] == '!') {
		cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
		kfree(trigger_data);
		ret = 0;
		goto out;
	}

	if (trigger) {
		number = strsep(&trigger, ":");

		ret = -EINVAL;
		if (!strlen(number))
			goto out_free;

		/*
		 * We use the callback data field (which is a pointer)
		 * as our counter.
		 */
		ret = kstrtoul(number, 0, &trigger_data->count);
		if (ret)
			goto out_free;
	}

	if (!param) /* if param is non-empty, it's supposed to be a filter */
		goto out_reg;

	if (!cmd_ops->set_filter)
		goto out_reg;

	ret = cmd_ops->set_filter(param, trigger_data, file);
	if (ret < 0)
		goto out_free;

 out_reg:
	ret = cmd_ops->reg(glob, trigger_ops, trigger_data, file);
	/*
	 * The above returns on success the # of functions enabled,
	 * but if it didn't find any functions it returns zero.
	 * Consider no functions a failure too.
	 */
	if (!ret) {
		ret = -ENOENT;
		goto out_free;
	} else if (ret < 0)
		goto out_free;
	ret = 0;
 out:
	return ret;

 out_free:
	if (cmd_ops->set_filter)
		cmd_ops->set_filter(NULL, trigger_data, NULL);
	kfree(trigger_data);
	goto out;
}

/**
 * set_trigger_filter - Generic event_command @set_filter implementation
 * @filter_str: The filter string for the trigger, NULL to remove filter
 * @trigger_data: Trigger-specific data
 * @file: The ftrace_event_file associated with the event
 *
 * Common implementation for event command filter parsing and filter
 * instantiation.
 *
 * Usually used directly as the @set_filter method in event command
 * implementations.
 *
 * Also used to remove a filter (if filter_str = NULL).
 *
 * Return: 0 on success, errno otherwise
 */
static int set_trigger_filter(char *filter_str,
			      struct event_trigger_data *trigger_data,
			      struct ftrace_event_file *file)
{
	struct event_trigger_data *data = trigger_data;
	struct event_filter *filter = NULL, *tmp;
	int ret = -EINVAL;
	char *s;

	if (!filter_str) /* clear the current filter */
		goto assign;

	s = strsep(&filter_str, " \t");

	if (!strlen(s) || strcmp(s, "if") != 0)
		goto out;

	if (!filter_str)
		goto out;

	/* The filter is for the 'trigger' event, not the triggered event */
	ret = create_event_filter(file->event_call, filter_str, false, &filter);
	if (ret)
		goto out;
 assign:
	tmp = rcu_access_pointer(data->filter);

	rcu_assign_pointer(data->filter, filter);

	if (tmp) {
		/* Make sure the call is done with the filter */
		synchronize_sched();
		free_event_filter(tmp);
	}

	kfree(data->filter_str);
	data->filter_str = NULL;

	if (filter_str) {
		data->filter_str = kstrdup(filter_str, GFP_KERNEL);
		if (!data->filter_str) {
			free_event_filter(rcu_access_pointer(data->filter));
			data->filter = NULL;
			ret = -ENOMEM;
		}
	}
 out:
	return ret;
}

static void
traceon_trigger(struct event_trigger_data *data, void *rec)
{
	if (tracing_is_on())
		return;

	tracing_on();
}

static void
traceon_count_trigger(struct event_trigger_data *data, void *rec)
{
	if (tracing_is_on())
		return;

	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	tracing_on();
}

static void
traceoff_trigger(struct event_trigger_data *data, void *rec)
{
	if (!tracing_is_on())
		return;

	tracing_off();
}

static void
traceoff_count_trigger(struct event_trigger_data *data, void *rec)
{
	if (!tracing_is_on())
		return;

	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	tracing_off();
}

static int
traceon_trigger_print(struct seq_file *m, struct event_trigger_ops *ops,
		      struct event_trigger_data *data)
{
	return event_trigger_print("traceon", m, (void *)data->count,
				   data->filter_str);
}

static int
traceoff_trigger_print(struct seq_file *m, struct event_trigger_ops *ops,
		       struct event_trigger_data *data)
{
	return event_trigger_print("traceoff", m, (void *)data->count,
				   data->filter_str);
}

static struct event_trigger_ops traceon_trigger_ops = {
	.func			= traceon_trigger,
	.print			= traceon_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops traceon_count_trigger_ops = {
	.func			= traceon_count_trigger,
	.print			= traceon_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops traceoff_trigger_ops = {
	.func			= traceoff_trigger,
	.print			= traceoff_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops traceoff_count_trigger_ops = {
	.func			= traceoff_count_trigger,
	.print			= traceoff_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops *
onoff_get_trigger_ops(char *cmd, char *param)
{
	struct event_trigger_ops *ops;

	/* we register both traceon and traceoff to this callback */
	if (strcmp(cmd, "traceon") == 0)
		ops = param ? &traceon_count_trigger_ops :
			&traceon_trigger_ops;
	else
		ops = param ? &traceoff_count_trigger_ops :
			&traceoff_trigger_ops;

	return ops;
}

static struct event_command trigger_traceon_cmd = {
	.name			= "traceon",
	.trigger_type		= ETT_TRACE_ONOFF,
	.func			= event_trigger_callback,
	.reg			= register_trigger,
	.unreg			= unregister_trigger,
	.get_trigger_ops	= onoff_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static struct event_command trigger_traceoff_cmd = {
	.name			= "traceoff",
	.trigger_type		= ETT_TRACE_ONOFF,
	.func			= event_trigger_callback,
	.reg			= register_trigger,
	.unreg			= unregister_trigger,
	.get_trigger_ops	= onoff_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

#ifdef CONFIG_TRACER_SNAPSHOT
static void
snapshot_trigger(struct event_trigger_data *data, void *rec)
{
	tracing_snapshot();
}

static void
snapshot_count_trigger(struct event_trigger_data *data, void *rec)
{
	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	snapshot_trigger(data, rec);
}

static int
register_snapshot_trigger(char *glob, struct event_trigger_ops *ops,
			  struct event_trigger_data *data,
			  struct ftrace_event_file *file)
{
	int ret = register_trigger(glob, ops, data, file);

	if (ret > 0 && tracing_alloc_snapshot() != 0) {
		unregister_trigger(glob, ops, data, file);
		ret = 0;
	}

	return ret;
}

static int
snapshot_trigger_print(struct seq_file *m, struct event_trigger_ops *ops,
		       struct event_trigger_data *data)
{
	return event_trigger_print("snapshot", m, (void *)data->count,
				   data->filter_str);
}

static struct event_trigger_ops snapshot_trigger_ops = {
	.func			= snapshot_trigger,
	.print			= snapshot_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops snapshot_count_trigger_ops = {
	.func			= snapshot_count_trigger,
	.print			= snapshot_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops *
snapshot_get_trigger_ops(char *cmd, char *param)
{
	return param ? &snapshot_count_trigger_ops : &snapshot_trigger_ops;
}

static struct event_command trigger_snapshot_cmd = {
	.name			= "snapshot",
	.trigger_type		= ETT_SNAPSHOT,
	.func			= event_trigger_callback,
	.reg			= register_snapshot_trigger,
	.unreg			= unregister_trigger,
	.get_trigger_ops	= snapshot_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init int register_trigger_snapshot_cmd(void)
{
	int ret;

	ret = register_event_command(&trigger_snapshot_cmd);
	WARN_ON(ret < 0);

	return ret;
}
#else
static __init int register_trigger_snapshot_cmd(void) { return 0; }
#endif /* CONFIG_TRACER_SNAPSHOT */

#ifdef CONFIG_STACKTRACE
/*
 * Skip 3:
 *   stacktrace_trigger()
 *   event_triggers_post_call()
 *   ftrace_raw_event_xxx()
 */
#define STACK_SKIP 3

static void
stacktrace_trigger(struct event_trigger_data *data, void *rec)
{
	trace_dump_stack(STACK_SKIP);
}

static void
stacktrace_count_trigger(struct event_trigger_data *data, void *rec)
{
	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	stacktrace_trigger(data, rec);
}

static int
stacktrace_trigger_print(struct seq_file *m, struct event_trigger_ops *ops,
			 struct event_trigger_data *data)
{
	return event_trigger_print("stacktrace", m, (void *)data->count,
				   data->filter_str);
}

static struct event_trigger_ops stacktrace_trigger_ops = {
	.func			= stacktrace_trigger,
	.print			= stacktrace_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops stacktrace_count_trigger_ops = {
	.func			= stacktrace_count_trigger,
	.print			= stacktrace_trigger_print,
	.init			= event_trigger_init,
	.free			= event_trigger_free,
};

static struct event_trigger_ops *
stacktrace_get_trigger_ops(char *cmd, char *param)
{
	return param ? &stacktrace_count_trigger_ops : &stacktrace_trigger_ops;
}

static struct event_command trigger_stacktrace_cmd = {
	.name			= "stacktrace",
	.trigger_type		= ETT_STACKTRACE,
	.post_trigger		= true,
	.func			= event_trigger_callback,
	.reg			= register_trigger,
	.unreg			= unregister_trigger,
	.get_trigger_ops	= stacktrace_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init int register_trigger_stacktrace_cmd(void)
{
	int ret;

	ret = register_event_command(&trigger_stacktrace_cmd);
	WARN_ON(ret < 0);

	return ret;
}
#else
static __init int register_trigger_stacktrace_cmd(void) { return 0; }
#endif /* CONFIG_STACKTRACE */

static __init void unregister_trigger_traceon_traceoff_cmds(void)
{
	unregister_event_command(&trigger_traceon_cmd);
	unregister_event_command(&trigger_traceoff_cmd);
}

/* Avoid typos */
#define ENABLE_EVENT_STR	"enable_event"
#define DISABLE_EVENT_STR	"disable_event"

struct enable_trigger_data {
	struct ftrace_event_file	*file;
	bool				enable;
};

static void
event_enable_trigger(struct event_trigger_data *data, void *rec)
{
	struct enable_trigger_data *enable_data = data->private_data;

	if (enable_data->enable)
		clear_bit(FTRACE_EVENT_FL_SOFT_DISABLED_BIT, &enable_data->file->flags);
	else
		set_bit(FTRACE_EVENT_FL_SOFT_DISABLED_BIT, &enable_data->file->flags);
}

static void
event_enable_count_trigger(struct event_trigger_data *data, void *rec)
{
	struct enable_trigger_data *enable_data = data->private_data;

	if (!data->count)
		return;

	/* Skip if the event is in a state we want to switch to */
	if (enable_data->enable == !(enable_data->file->flags & FTRACE_EVENT_FL_SOFT_DISABLED))
		return;

	if (data->count != -1)
		(data->count)--;

	event_enable_trigger(data, rec);
}

static int
event_enable_trigger_print(struct seq_file *m, struct event_trigger_ops *ops,
			   struct event_trigger_data *data)
{
	struct enable_trigger_data *enable_data = data->private_data;

	seq_printf(m, "%s:%s:%s",
		   enable_data->enable ? ENABLE_EVENT_STR : DISABLE_EVENT_STR,
		   enable_data->file->event_call->class->system,
		   enable_data->file->event_call->name);

	if (data->count == -1)
		seq_puts(m, ":unlimited");
	else
		seq_printf(m, ":count=%ld", data->count);

	if (data->filter_str)
		seq_printf(m, " if %s\n", data->filter_str);
	else
		seq_puts(m, "\n");

	return 0;
}

static void
event_enable_trigger_free(struct event_trigger_ops *ops,
			  struct event_trigger_data *data)
{
	struct enable_trigger_data *enable_data = data->private_data;

	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref) {
		/* Remove the SOFT_MODE flag */
		trace_event_enable_disable(enable_data->file, 0, 1);
		module_put(enable_data->file->event_call->mod);
		trigger_data_free(data);
		kfree(enable_data);
	}
}

static struct event_trigger_ops event_enable_trigger_ops = {
	.func			= event_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops event_enable_count_trigger_ops = {
	.func			= event_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops event_disable_trigger_ops = {
	.func			= event_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops event_disable_count_trigger_ops = {
	.func			= event_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static int
event_enable_trigger_func(struct event_command *cmd_ops,
			  struct ftrace_event_file *file,
			  char *glob, char *cmd, char *param)
{
	struct ftrace_event_file *event_enable_file;
	struct enable_trigger_data *enable_data;
	struct event_trigger_data *trigger_data;
	struct event_trigger_ops *trigger_ops;
	struct trace_array *tr = file->tr;
	const char *system;
	const char *event;
	char *trigger;
	char *number;
	bool enable;
	int ret;

	if (!param)
		return -EINVAL;

	/* separate the trigger from the filter (s:e:n [if filter]) */
	trigger = strsep(&param, " \t");
	if (!trigger)
		return -EINVAL;

	system = strsep(&trigger, ":");
	if (!trigger)
		return -EINVAL;

	event = strsep(&trigger, ":");

	ret = -EINVAL;
	event_enable_file = find_event_file(tr, system, event);
	if (!event_enable_file)
		goto out;

	enable = strcmp(cmd, ENABLE_EVENT_STR) == 0;

	trigger_ops = cmd_ops->get_trigger_ops(cmd, trigger);

	ret = -ENOMEM;
	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data)
		goto out;

	enable_data = kzalloc(sizeof(*enable_data), GFP_KERNEL);
	if (!enable_data) {
		kfree(trigger_data);
		goto out;
	}

	trigger_data->count = -1;
	trigger_data->ops = trigger_ops;
	trigger_data->cmd_ops = cmd_ops;
	INIT_LIST_HEAD(&trigger_data->list);
	RCU_INIT_POINTER(trigger_data->filter, NULL);

	enable_data->enable = enable;
	enable_data->file = event_enable_file;
	trigger_data->private_data = enable_data;

	if (glob[0] == '!') {
		cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
		kfree(trigger_data);
		kfree(enable_data);
		ret = 0;
		goto out;
	}

	if (trigger) {
		number = strsep(&trigger, ":");

		ret = -EINVAL;
		if (!strlen(number))
			goto out_free;

		/*
		 * We use the callback data field (which is a pointer)
		 * as our counter.
		 */
		ret = kstrtoul(number, 0, &trigger_data->count);
		if (ret)
			goto out_free;
	}

	if (!param) /* if param is non-empty, it's supposed to be a filter */
		goto out_reg;

	if (!cmd_ops->set_filter)
		goto out_reg;

	ret = cmd_ops->set_filter(param, trigger_data, file);
	if (ret < 0)
		goto out_free;

 out_reg:
	/* Don't let event modules unload while probe registered */
	ret = try_module_get(event_enable_file->event_call->mod);
	if (!ret) {
		ret = -EBUSY;
		goto out_free;
	}

	ret = trace_event_enable_disable(event_enable_file, 1, 1);
	if (ret < 0)
		goto out_put;
	ret = cmd_ops->reg(glob, trigger_ops, trigger_data, file);
	/*
	 * The above returns on success the # of functions enabled,
	 * but if it didn't find any functions it returns zero.
	 * Consider no functions a failure too.
	 */
	if (!ret) {
		ret = -ENOENT;
		goto out_disable;
	} else if (ret < 0)
		goto out_disable;
	/* Just return zero, not the number of enabled functions */
	ret = 0;
 out:
	return ret;

 out_disable:
	trace_event_enable_disable(event_enable_file, 0, 1);
 out_put:
	module_put(event_enable_file->event_call->mod);
 out_free:
	if (cmd_ops->set_filter)
		cmd_ops->set_filter(NULL, trigger_data, NULL);
	kfree(trigger_data);
	kfree(enable_data);
	goto out;
}

static int event_enable_register_trigger(char *glob,
					 struct event_trigger_ops *ops,
					 struct event_trigger_data *data,
					 struct ftrace_event_file *file)
{
	struct enable_trigger_data *enable_data = data->private_data;
	struct enable_trigger_data *test_enable_data;
	struct event_trigger_data *test;
	int ret = 0;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		test_enable_data = test->private_data;
		if (test_enable_data &&
		    (test_enable_data->file == enable_data->file)) {
			ret = -EEXIST;
			goto out;
		}
	}

	if (data->ops->init) {
		ret = data->ops->init(data->ops, data);
		if (ret < 0)
			goto out;
	}

	list_add_rcu(&data->list, &file->triggers);
	ret++;

	if (trace_event_trigger_enable_disable(file, 1) < 0) {
		list_del_rcu(&data->list);
		ret--;
	}
	update_cond_flag(file);
out:
	return ret;
}

static void event_enable_unregister_trigger(char *glob,
					    struct event_trigger_ops *ops,
					    struct event_trigger_data *test,
					    struct ftrace_event_file *file)
{
	struct enable_trigger_data *test_enable_data = test->private_data;
	struct enable_trigger_data *enable_data;
	struct event_trigger_data *data;
	bool unregistered = false;

	list_for_each_entry_rcu(data, &file->triggers, list) {
		enable_data = data->private_data;
		if (enable_data &&
		    (enable_data->file == test_enable_data->file)) {
			unregistered = true;
			list_del_rcu(&data->list);
			update_cond_flag(file);
			trace_event_trigger_enable_disable(file, 0);
			break;
		}
	}

	if (unregistered && data->ops->free)
		data->ops->free(data->ops, data);
}

static struct event_trigger_ops *
event_enable_get_trigger_ops(char *cmd, char *param)
{
	struct event_trigger_ops *ops;
	bool enable;

	enable = strcmp(cmd, ENABLE_EVENT_STR) == 0;

	if (enable)
		ops = param ? &event_enable_count_trigger_ops :
			&event_enable_trigger_ops;
	else
		ops = param ? &event_disable_count_trigger_ops :
			&event_disable_trigger_ops;

	return ops;
}

static struct event_command trigger_enable_cmd = {
	.name			= ENABLE_EVENT_STR,
	.trigger_type		= ETT_EVENT_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.get_trigger_ops	= event_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static struct event_command trigger_disable_cmd = {
	.name			= DISABLE_EVENT_STR,
	.trigger_type		= ETT_EVENT_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.get_trigger_ops	= event_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init void unregister_trigger_enable_disable_cmds(void)
{
	unregister_event_command(&trigger_enable_cmd);
	unregister_event_command(&trigger_disable_cmd);
}

static __init int register_trigger_enable_disable_cmds(void)
{
	int ret;

	ret = register_event_command(&trigger_enable_cmd);
	if (WARN_ON(ret < 0))
		return ret;
	ret = register_event_command(&trigger_disable_cmd);
	if (WARN_ON(ret < 0))
		unregister_trigger_enable_disable_cmds();

	return ret;
}

static __init int register_trigger_traceon_traceoff_cmds(void)
{
	int ret;

	ret = register_event_command(&trigger_traceon_cmd);
	if (WARN_ON(ret < 0))
		return ret;
	ret = register_event_command(&trigger_traceoff_cmd);
	if (WARN_ON(ret < 0))
		unregister_trigger_traceon_traceoff_cmds();

	return ret;
}

struct hash_field;

typedef u64 (*hash_field_fn_t) (struct hash_field *field, void *event);

struct hash_field {
	struct ftrace_event_field	*field;
	struct ftrace_event_field	*aux_field;
	hash_field_fn_t			fn;
	unsigned long			flags;
};

static u64 hash_field_none(struct hash_field *field, void *event)
{
	return 0;
}

static u64 hash_field_string(struct hash_field *hash_field, void *event)
{
	char *addr = (char *)(event + hash_field->field->offset);

	return (u64)addr;
}

static u64 hash_field_diff(struct hash_field *hash_field, void *event)
{
	u64 *m, *s;

	m = (u64 *)(event + hash_field->field->offset);
	s = (u64 *)(event + hash_field->aux_field->offset);

	return *m - *s;
}

#define DEFINE_HASH_FIELD_FN(type)					\
static u64 hash_field_##type(struct hash_field *hash_field, void *event)\
{									\
	type *addr = (type *)(event + hash_field->field->offset);	\
									\
	return (u64)*addr;						\
}

DEFINE_HASH_FIELD_FN(s64);
DEFINE_HASH_FIELD_FN(u64);
DEFINE_HASH_FIELD_FN(s32);
DEFINE_HASH_FIELD_FN(u32);
DEFINE_HASH_FIELD_FN(s16);
DEFINE_HASH_FIELD_FN(u16);
DEFINE_HASH_FIELD_FN(s8);
DEFINE_HASH_FIELD_FN(u8);

#define HASH_TRIGGER_BYTES	(2621440 * 2)

/* enough memory for one hashtrigger of bits 12 */
static char hashtrigger_bytes[HASH_TRIGGER_BYTES];
static char *hashtrigger_bytes_alloc = hashtrigger_bytes;

static void *hash_data_kzalloc(unsigned long size)
{
	return kzalloc(size, GFP_KERNEL);
}

static void *hash_data_bootmem_alloc(unsigned long size)
{
	char *ptr = hashtrigger_bytes_alloc;

	if (hashtrigger_bytes_alloc + size > hashtrigger_bytes + HASH_TRIGGER_BYTES)
		return NULL;

	hashtrigger_bytes_alloc += size;

	return ptr;
}

static void hash_data_kfree(void *obj)
{
	return kfree(obj);
}

static void hash_data_bootmem_free(void *obj)
{
}

static char *hash_data_kstrdup(const char *str)
{
	return kstrdup(str, GFP_KERNEL);
}

static char *hash_data_bootmem_strdup(const char *str)
{
	char *newstr;

	newstr = hash_data_bootmem_alloc(strlen(str) + 1);
	strcpy(newstr, str);

	return newstr;
}

typedef void *(*hash_data_alloc_fn_t) (unsigned long size);
typedef void (*hash_data_free_fn_t) (void *obj);
typedef char *(*hash_data_strdup_fn_t) (const char *str);

#define HASH_TRIGGER_BITS	11
#define COMPOUND_KEY_MAX	8
#define HASH_VALS_MAX		16
#define HASH_SORT_KEYS_MAX	2

/* Largest event field string currently 32, add 1 = 64 */
#define HASH_KEY_STRING_MAX	64

/* subsys:name */
#define MAX_EVENT_NAME_LEN	128

enum hash_field_flags {
	HASH_FIELD_SYM		= 1,
	HASH_FIELD_HEX		= 2,
	HASH_FIELD_STACKTRACE	= 4,
	HASH_FIELD_STRING	= 8,
	HASH_FIELD_EXECNAME	= 16,
	HASH_FIELD_SYSCALL	= 32,
	HASH_FIELD_OVERRIDE	= 64,
};

enum sort_key_flags {
	SORT_KEY_COUNT		= 1,
};

struct hash_trigger_sort_key {
	bool		descending;
	bool		use_hitcount;
	bool		key_part;
	unsigned int	idx;
};

struct hash_trigger_data {
	struct	hlist_head		*hashtab;
	unsigned int			hashtab_bits;
	char				early_event_name[MAX_EVENT_NAME_LEN];
	char				*keys_str;
	char				*vals_str;
	char				*sort_keys_str;
	struct hash_field		*keys[COMPOUND_KEY_MAX];
	unsigned int			n_keys;
	struct hash_field		*vals[HASH_VALS_MAX];
	unsigned int			n_vals;
	struct ftrace_event_file	*event_file;
	unsigned long			total_hits;
	unsigned long			total_entries;
	struct hash_trigger_sort_key	*sort_keys[HASH_SORT_KEYS_MAX];
	struct hash_trigger_sort_key	*sort_key_cur;
	spinlock_t			lock;
	unsigned int			max_entries;
	struct hash_trigger_entry	*entries;
	unsigned int			n_entries;
	struct stack_trace		*struct_stacktrace_entries;
	unsigned int			n_struct_stacktrace_entries;
	unsigned long			*stacktrace_entries;
	unsigned int			n_stacktrace_entries;
	char				*hash_key_string_entries;
	unsigned int			n_hash_key_string_entries;
	unsigned long			drops;
};

enum hash_key_type {
	HASH_KEY_TYPE_U64,
	HASH_KEY_TYPE_STACKTRACE,
	HASH_KEY_TYPE_STRING,
};

struct hash_key_part {
	enum hash_key_type		type;
	unsigned long			flags;
	union {
		u64			val_u64;
		struct stack_trace	*val_stacktrace;
		char			*val_string;
	} var;
};

struct hash_trigger_entry {
	struct	hlist_node		node;
	struct hash_key_part		key_parts[COMPOUND_KEY_MAX];
	u64				sums[HASH_VALS_MAX];
	char				comm[TASK_COMM_LEN + 1];
	u64				count;
	struct hash_trigger_data*	hash_data;
};

#define HASH_STACKTRACE_DEPTH 16
#define HASH_STACKTRACE_SKIP 3

static hash_field_fn_t select_value_fn(int field_size, int field_is_signed)
{
	hash_field_fn_t fn = NULL;

	switch (field_size) {
	case 8:
		if (field_is_signed)
			fn = hash_field_s64;
		else
			fn = hash_field_u64;
		break;
	case 4:
		if (field_is_signed)
			fn = hash_field_s32;
		else
			fn = hash_field_u32;
		break;
	case 2:
		if (field_is_signed)
			fn = hash_field_s16;
		else
			fn = hash_field_u16;
		break;
	case 1:
		if (field_is_signed)
			fn = hash_field_s8;
		else
			fn = hash_field_u8;
		break;
	}

	return fn;
}

#define FNV_OFFSET_BASIS	(14695981039346656037ULL)
#define FNV_PRIME		(1099511628211ULL)

static u64 hash_fnv_1a(char *key, unsigned int size, unsigned int bits)
{
	u64 hash = FNV_OFFSET_BASIS;
	unsigned int i;

	for (i = 0; i < size; i++) {
		hash ^= key[i];
		hash *= FNV_PRIME;
	}

	return hash >> (64 - bits);
}

static u64 hash_stacktrace(struct stack_trace *stacktrace, unsigned int bits)
{
	unsigned int size;

	size = stacktrace->nr_entries * sizeof(*stacktrace->entries);

	return hash_fnv_1a((char *)stacktrace->entries, size, bits);
}

static u64 hash_string(struct hash_field *hash_field,
		       unsigned int bits, void *rec)
{
	unsigned int size;
	char *string;

	size = hash_field->field->size;
	string = (char *)hash_field->fn(hash_field, rec);

	return hash_fnv_1a(string, size, bits);
}

static u64 hash_compound_key(struct hash_trigger_data *hash_data,
			     unsigned int bits, void *rec)
{
	struct hash_field *hash_field;
	u64 key[COMPOUND_KEY_MAX];
	unsigned int i;

	for (i = 0; i < hash_data->n_keys; i++) {
		hash_field = hash_data->keys[i];
		key[i] = hash_field->fn(hash_field, rec);
	}

	return hash_fnv_1a((char *)key, hash_data->n_keys * sizeof(key[0]), bits);
}

static u64 hash_key(struct hash_trigger_data *hash_data, void *rec,
		    struct stack_trace *stacktrace)
{
	/* currently can't have compound key with string or stacktrace */
	struct hash_field *hash_field = hash_data->keys[0];
	unsigned int bits = hash_data->hashtab_bits;
	u64 hash_idx = 0;

	if (hash_field->flags & HASH_FIELD_STACKTRACE)
		hash_idx = hash_stacktrace(stacktrace, bits);
	else if (hash_field->flags & HASH_FIELD_STRING)
		hash_idx = hash_string(hash_field, bits, rec);
	else if (hash_data->n_keys > 1)
		hash_idx = hash_compound_key(hash_data, bits, rec);
	else {
		u64 hash_val = hash_field->fn(hash_field, rec);

		switch (hash_field->field->size) {
		case 8:
			hash_idx = hash_64(hash_val, bits);
			break;
		case 4:
			hash_idx = hash_32(hash_val, bits);
			break;
		default:
			WARN_ON_ONCE(1);
			break;
		}
	}

	return hash_idx;
}

static inline void save_comm(char *comm, struct task_struct *task) {

	if (!task->pid) {
		strcpy(comm, "<idle>");
		return;
	}

	if (WARN_ON_ONCE(task->pid < 0)) {
		strcpy(comm, "<XXX>");
		return;
	}

	if (task->pid > PID_MAX_DEFAULT) {
		strcpy(comm, "<...>");
		return;
	}

	memcpy(comm, task->comm, TASK_COMM_LEN);
}

static void stacktrace_entry_fill(struct hash_trigger_entry *entry,
				  unsigned int key,
				  struct hash_field *hash_field,
				  struct stack_trace *stacktrace)
{
	struct hash_trigger_data *hash_data = entry->hash_data;
	struct stack_trace *stacktrace_copy;
	unsigned int size, offset, idx;

	idx = hash_data->n_struct_stacktrace_entries++;
	stacktrace_copy = &hash_data->struct_stacktrace_entries[idx];
	*stacktrace_copy = *stacktrace;

	idx = hash_data->n_stacktrace_entries++;
	size = sizeof(unsigned long) * HASH_STACKTRACE_DEPTH;
	offset = HASH_STACKTRACE_DEPTH * idx;
	stacktrace_copy->entries = &hash_data->stacktrace_entries[offset];
	memcpy(stacktrace_copy->entries, stacktrace->entries, size);

	entry->key_parts[key].type = HASH_KEY_TYPE_STACKTRACE;
	entry->key_parts[key].flags = hash_field->flags;
	entry->key_parts[key].var.val_stacktrace = stacktrace_copy;
}

static void string_entry_fill(struct hash_trigger_entry *entry,
			      unsigned int key,
			      struct hash_field *hash_field,
			      void *rec)
{
	struct hash_trigger_data *hash_data = entry->hash_data;
	unsigned int size = hash_field->field->size + 1;
	unsigned int offset;
	char *string_copy;

	offset = HASH_KEY_STRING_MAX * hash_data->n_hash_key_string_entries++;
	string_copy = &hash_data->hash_key_string_entries[offset];

	memcpy(string_copy, (char *)hash_field->fn(hash_field, rec), size);

	entry->key_parts[key].type = HASH_KEY_TYPE_STRING;
	entry->key_parts[key].flags = hash_field->flags;
	entry->key_parts[key].var.val_string = string_copy;
}

static struct hash_trigger_entry *
hash_trigger_entry_create(struct hash_trigger_data *hash_data, void *rec,
			  struct stack_trace *stacktrace)
{
	struct hash_trigger_entry *entry = NULL;
	struct hash_field *hash_field;
	bool save_execname = false;
	unsigned int i;

	if (hash_data->drops)
		return NULL;
	else if (hash_data->n_entries == hash_data->max_entries) {
		hash_data->drops = 1;
		return NULL;
	}

	entry = &hash_data->entries[hash_data->n_entries++];
	if (!entry)
		return NULL;

	entry->hash_data = hash_data;

	for (i = 0; i < hash_data->n_keys; i++) {
		hash_field = hash_data->keys[i];

		if (hash_field->flags & HASH_FIELD_STACKTRACE)
			stacktrace_entry_fill(entry, i, hash_field, stacktrace);
		else if (hash_field->flags & HASH_FIELD_STRING)
			string_entry_fill(entry, i, hash_field, rec);
		else {
			u64 hash_val = hash_field->fn(hash_field, rec);

			entry->key_parts[i].type = HASH_KEY_TYPE_U64;
			entry->key_parts[i].flags = hash_field->flags;
			entry->key_parts[i].var.val_u64 = hash_val;
			/*
			  EXECNAME only applies to common_pid as a
			  key, And with the assumption that the comm
			  saved is only for common_pid i.e. current
			  pid when the event was logged.  comm is
			  saved only when the hash entry is created,
			  subsequent hits for that hash entry map the
			  same pid and comm.
			*/
			if (hash_field->flags & HASH_FIELD_EXECNAME)
				save_execname = true;
		}
	}

	if (save_execname)
		save_comm(entry->comm, current);

	return entry;
}

static void destroy_hashtab(struct hash_trigger_data *hash_data,
			    hash_data_free_fn_t free_fn)
{
	struct hlist_head *hashtab = hash_data->hashtab;

	if (!hashtab)
		return;

	free_fn(hashtab);

	hash_data->hashtab = NULL;
}

static void destroy_hash_field(struct hash_field *hash_field,
			       hash_data_free_fn_t free_fn)
{
	free_fn(hash_field);
}

static struct hash_field *create_hash_field(struct ftrace_event_field *field,
					    struct ftrace_event_field *aux_field,
					    unsigned long flags,
					    hash_data_alloc_fn_t alloc_fn,
					    hash_data_free_fn_t free_fn)
{
	hash_field_fn_t fn = hash_field_none;
	struct hash_field *hash_field;

	hash_field = alloc_fn(sizeof(struct hash_field));
	if (!hash_field)
		return NULL;

	if (flags & HASH_FIELD_STACKTRACE) {
		hash_field->flags = flags;
		goto out;
	}

	if (flags & HASH_FIELD_OVERRIDE) {
		hash_field->flags = flags;
		goto out;
	}

	if (is_string_field(field)) {
		flags |= HASH_FIELD_STRING;
		fn = hash_field_string;
	} else if (is_function_field(field))
		goto free;
	else {
		if (aux_field) {
			hash_field->aux_field = aux_field;
			fn = hash_field_diff;
		} else {
			fn = select_value_fn(field->size, field->is_signed);
			if (!fn)
				goto free;
		}
	}

	hash_field->field = field;
	hash_field->fn = fn;
	hash_field->flags = flags;
 out:
	return hash_field;
 free:
	free_fn(hash_field);
	hash_field = NULL;
	goto out;
}

static void destroy_hash_fields(struct hash_trigger_data *hash_data,
				hash_data_free_fn_t free_fn)
{
	unsigned int i;

	for (i = 0; i < hash_data->n_keys; i++) {
		destroy_hash_field(hash_data->keys[i], free_fn);
		hash_data->keys[i] = NULL;
	}

	for (i = 0; i < hash_data->n_vals; i++) {
		destroy_hash_field(hash_data->vals[i], free_fn);
		hash_data->vals[i] = NULL;
	}
}

static inline struct hash_trigger_sort_key *create_default_sort_key(hash_data_alloc_fn_t alloc_fn)
{
	struct hash_trigger_sort_key *sort_key;

	sort_key = alloc_fn(sizeof(*sort_key));
	if (!sort_key)
		return NULL;

	sort_key->use_hitcount = true;

	return sort_key;
}

static inline struct hash_trigger_sort_key *
create_sort_key(char *field_name, struct hash_trigger_data *hash_data,
		hash_data_alloc_fn_t alloc_fn,
		hash_data_free_fn_t free_fn)
{
	struct hash_trigger_sort_key *sort_key;
	bool key_part = false;
	unsigned int j;

	if (!strcmp(field_name, "hitcount"))
		return create_default_sort_key(alloc_fn);

	if (strchr(field_name, '-')) {
		char *aux_field_name = field_name;

		field_name = strsep(&aux_field_name, "-");
		if (!aux_field_name)
			return NULL;

		for (j = 0; j < hash_data->n_vals; j++)
			if (!strcmp(field_name,
				    hash_data->vals[j]->field->name) &&
			    (hash_data->vals[j]->aux_field &&
			     !strcmp(aux_field_name,
				     hash_data->vals[j]->aux_field->name)))
				goto out;
	}

	for (j = 0; j < hash_data->n_vals; j++)
		if (!strcmp(field_name, hash_data->vals[j]->field->name))
			goto out;

	for (j = 0; j < hash_data->n_keys; j++) {
		if (hash_data->keys[j]->flags & HASH_FIELD_STACKTRACE)
			continue;
		if (hash_data->keys[j]->flags & HASH_FIELD_STRING)
			continue;
		if (!strcmp(field_name, hash_data->keys[j]->field->name)) {
			key_part = true;
			goto out;
		}
	}

	return NULL;
 out:
	sort_key = alloc_fn(sizeof(*sort_key));
	if (!sort_key)
		return NULL;

	sort_key->idx = j;
	sort_key->key_part = key_part;

	return sort_key;
}

static int create_sort_keys(struct hash_trigger_data *hash_data,
			    hash_data_alloc_fn_t alloc_fn,
			    hash_data_free_fn_t free_fn)
{
	char *fields_str = hash_data->sort_keys_str;
	struct hash_trigger_sort_key *sort_key;
	char *field_str, *field_name;
	unsigned int i;
	int ret = 0;

	if (!fields_str) {
		sort_key = create_default_sort_key(alloc_fn);
		if (!sort_key) {
			ret = -ENOMEM;
			goto out;
		}
		hash_data->sort_keys[0] = sort_key;
		goto out;
	}

	strsep(&fields_str, "=");
	if (!fields_str) {
		ret = -EINVAL;
		goto free;
	}

	for (i = 0; i < HASH_SORT_KEYS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str) {
			if (i == 0) {
				ret = -EINVAL;
				goto free;
			} else
				break;
		}

		field_name = strsep(&field_str, ".");
		sort_key = create_sort_key(field_name, hash_data, alloc_fn, free_fn);
		if (!sort_key) {
			ret = -EINVAL; /* or -ENOMEM */
			goto free;
		}
		if (field_str) {
			if (!strcmp(field_str, "descending"))
				sort_key->descending = true;
			else if (strcmp(field_str, "ascending")) {
				ret = -EINVAL; /* not either, err */
				goto free;
			}
		}
		hash_data->sort_keys[i] = sort_key;
	}
out:
	return ret;
free:
	for (i = 0; i < HASH_SORT_KEYS_MAX; i++) {
		if (!hash_data->sort_keys[i])
			break;
		free_fn(hash_data->sort_keys[i]);
		hash_data->sort_keys[i] = NULL;
	}
	goto out;
}

static int create_key_field(struct hash_trigger_data *hash_data,
			    unsigned int key,
			    struct ftrace_event_file *file,
			    char *field_str,
			    hash_data_alloc_fn_t alloc_fn,
			    hash_data_free_fn_t free_fn)
{
	struct ftrace_event_field *field = NULL;
	unsigned long flags = 0;
	char *field_name;
	int ret = 0;

	if (!strcmp(field_str, "stacktrace")) {
		flags |= HASH_FIELD_STACKTRACE;
	} else {
		field_name = strsep(&field_str, ".");
		if (field_str) {
			if (!strcmp(field_str, "sym"))
				flags |= HASH_FIELD_SYM;
			else if (!strcmp(field_str, "hex"))
				flags |= HASH_FIELD_HEX;
			else if (!strcmp(field_str, "execname") &&
				 !strcmp(field_name, "common_pid"))
				flags |= HASH_FIELD_EXECNAME;
			else if (!strcmp(field_str, "syscall"))
				flags |= HASH_FIELD_SYSCALL;
		}

		field = trace_find_event_field(file->event_call, field_name);
		if (!field) {
			ret = -EINVAL;
			goto out;
		}
	}

	hash_data->keys[key] = create_hash_field(field, NULL, flags,
						 alloc_fn, free_fn);
	if (!hash_data->keys[key]) {
		ret = -ENOMEM;
		goto out;
	}
	hash_data->n_keys++;
 out:
	return ret;
}

static int create_val_field(struct hash_trigger_data *hash_data,
			    unsigned int val,
			    struct ftrace_event_file *file,
			    char *field_str,
			    hash_data_alloc_fn_t alloc_fn,
			    hash_data_free_fn_t free_fn)
{
	struct ftrace_event_field *field = NULL;
	unsigned long flags = 0;
	char *field_name;
	int ret = 0;

	if (!strcmp(field_str, "hitcount"))
		return ret; /* There's always a hitcount */

	field_name = strsep(&field_str, "-");
	if (field_str) {
		struct ftrace_event_field *m_field, *s_field;

		m_field = trace_find_event_field(file->event_call, field_name);
		if (!m_field || is_string_field(m_field) ||
		    is_function_field(m_field)) {
			ret = -EINVAL;
			goto out;
		}

		s_field = trace_find_event_field(file->event_call, field_str);
		if (!s_field || is_string_field(m_field) ||
		    is_function_field(m_field)) {
			ret = -EINVAL;
			goto out;
		}

		hash_data->vals[val] = create_hash_field(m_field, s_field,
							 flags, alloc_fn,
							 free_fn);
		if (!hash_data->vals[val]) {
			ret = -ENOMEM;
			goto out;
		}
	} else {
		field_str = field_name;
		field_name = strsep(&field_str, ".");

		if (field_str) {
			if (!strcmp(field_str, "sym"))
				flags |= HASH_FIELD_SYM;
			else if (!strcmp(field_str, "hex"))
				flags |= HASH_FIELD_HEX;
			else if (!strcmp(field_str, "override"))
				flags |= HASH_FIELD_OVERRIDE;
		}

		if (!(flags & HASH_FIELD_OVERRIDE)) {
			field = trace_find_event_field(file->event_call,
						       field_name);
			if (!field) {
				ret = -EINVAL;
				goto out;
			}
		}

		hash_data->vals[val] = create_hash_field(field, NULL, flags,
							 alloc_fn, free_fn);
		if (!hash_data->vals[val]) {
			ret = -ENOMEM;
			goto out;
		}
	}
	hash_data->n_vals++;
 out:
	return ret;
}

static int create_hash_fields(struct hash_trigger_data *hash_data,
			      struct ftrace_event_file *file,
			      hash_data_alloc_fn_t alloc_fn,
			      hash_data_free_fn_t free_fn)
{
	char *fields_str, *field_str;
	unsigned int i;
	int ret = 0;

	fields_str = hash_data->keys_str;

	for (i = 0; i < COMPOUND_KEY_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str) {
			if (i == 0) {
				ret = -EINVAL;
				goto out;
			} else
				break;
		}

		ret = create_key_field(hash_data, i, file, field_str,
				       alloc_fn, free_fn);
		if (ret)
			goto out;
	}

	fields_str = hash_data->vals_str;

	for (i = 0; i < HASH_VALS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str) {
			if (i == 0) {
				ret = -EINVAL;
				goto out;
			} else
				break;
		}

		ret = create_val_field(hash_data, i, file, field_str,
				       alloc_fn, free_fn);
		if (ret)
			goto out;
	}

	ret = create_sort_keys(hash_data, alloc_fn, free_fn);
 out:
	return ret;
}

static void destroy_hashdata(struct hash_trigger_data *hash_data,
			     hash_data_free_fn_t free_fn)
{
	synchronize_sched();

	free_fn(hash_data->keys_str);
	free_fn(hash_data->vals_str);
	free_fn(hash_data->sort_keys_str);
	hash_data->keys_str = NULL;
	hash_data->vals_str = NULL;
	hash_data->sort_keys_str = NULL;

	free_fn(hash_data->entries);
	hash_data->entries = NULL;

	free_fn(hash_data->struct_stacktrace_entries);
	hash_data->struct_stacktrace_entries = NULL;

	free_fn(hash_data->stacktrace_entries);
	hash_data->stacktrace_entries = NULL;

	free_fn(hash_data->hash_key_string_entries);
	hash_data->hash_key_string_entries = NULL;

	destroy_hash_fields(hash_data, free_fn);
	destroy_hashtab(hash_data, free_fn);

	free_fn(hash_data);
}

static struct hash_trigger_data *create_hash_data(unsigned int hashtab_bits,
						  const char *keys,
						  const char *vals,
						  const char *sort_keys,
						  struct ftrace_event_file *file,
						  hash_data_alloc_fn_t alloc_fn,
						  hash_data_free_fn_t free_fn,
						  hash_data_strdup_fn_t strdup_fn,
						  int *ret)
{
	unsigned int hashtab_size = (1 << hashtab_bits);
	struct hash_trigger_data *hash_data;
	unsigned int i, size;

	hash_data = alloc_fn(sizeof(*hash_data));
	if (!hash_data)
		return NULL;

	/* Let's just say we size for a perfect hash but are not
	 * perfect. So let's have enough for 2 * the hashtab_size. */

	/* Also, we'll run out of entries before or at the same time
	 * we run out of other items like strings or stacks, so we
	 * only need to pay attention to one counter, for entries. */

	/* Also, use vmalloc or something for these large blocks. */
	hash_data->max_entries = hashtab_size * 2;
	size = sizeof(struct hash_trigger_entry) * hash_data->max_entries;
	hash_data->entries = alloc_fn(size);
	if (!hash_data->entries)
		goto free;

	size = sizeof(struct stack_trace) * hash_data->max_entries;
	hash_data->struct_stacktrace_entries = alloc_fn(size);
	if (!hash_data->struct_stacktrace_entries)
		goto free;

	size = sizeof(unsigned long) * HASH_STACKTRACE_DEPTH * hash_data->max_entries;
	hash_data->stacktrace_entries = alloc_fn(size);
	if (!hash_data->stacktrace_entries)
		goto free;

	size = sizeof(char) * HASH_KEY_STRING_MAX * hash_data->max_entries;
	hash_data->hash_key_string_entries = alloc_fn(size);
	if (!hash_data->hash_key_string_entries)
		goto free;

	hash_data->keys_str = strdup_fn(keys);
	hash_data->vals_str = strdup_fn(vals);
	if (sort_keys)
		hash_data->sort_keys_str = strdup_fn(sort_keys);

	*ret = create_hash_fields(hash_data, file, alloc_fn, free_fn);
	if (*ret < 0)
		goto free;

	hash_data->hashtab = alloc_fn(hashtab_size * sizeof(struct hlist_head));
	if (!hash_data->hashtab) {
		*ret = -ENOMEM;
		goto free;
	}

	for (i = 0; i < hashtab_size; i++)
		INIT_HLIST_HEAD(&hash_data->hashtab[i]);
	spin_lock_init(&hash_data->lock);

	hash_data->hashtab_bits = hashtab_bits;
	hash_data->event_file = file;
 out:
	return hash_data;
 free:
	destroy_hashdata(hash_data, free_fn);
	hash_data = NULL;
	goto out;
}

static inline bool match_stacktraces(struct stack_trace *entry_stacktrace,
				     struct stack_trace *stacktrace)
{
	unsigned int size;

	if (entry_stacktrace->nr_entries != entry_stacktrace->nr_entries)
		return false;

	size = sizeof(*stacktrace->entries) * stacktrace->nr_entries;
	if (memcmp(entry_stacktrace->entries, stacktrace->entries, size) == 0)
		return true;

	return false;
}

static struct hash_trigger_entry *
hash_trigger_entry_match(struct hash_trigger_entry *entry,
			 struct hash_key_part *key_parts,
			 unsigned int n_key_parts)
{
	unsigned int i;

	for (i = 0; i < n_key_parts; i++) {
		if (entry->key_parts[i].type != key_parts[i].type)
			return NULL;

		switch (entry->key_parts[i].type) {
		case HASH_KEY_TYPE_U64:
			if (entry->key_parts[i].var.val_u64 !=
			    key_parts[i].var.val_u64)
				return NULL;
			break;
		case HASH_KEY_TYPE_STACKTRACE:
			if (!match_stacktraces(entry->key_parts[i].var.val_stacktrace,
					       key_parts[i].var.val_stacktrace))
				return NULL;
			break;
		case HASH_KEY_TYPE_STRING:
			if (strcmp(entry->key_parts[i].var.val_string,
				   key_parts[i].var.val_string))
				return NULL;
			break;
		default:
			return NULL;
		}
	}

	return entry;
}

static struct hash_trigger_entry *
hash_trigger_entry_find(struct hash_trigger_data *hash_data, void *rec,
			struct stack_trace *stacktrace)
{
	struct hash_key_part key_parts[COMPOUND_KEY_MAX];
	unsigned int i, n_keys = hash_data->n_keys;
	struct hash_trigger_entry *entry;
	struct hash_field *hash_field;
	u64 hash_idx;

	hash_idx = hash_key(hash_data, rec, stacktrace);

	for (i = 0; i < n_keys; i++) {
		hash_field = hash_data->keys[i];
		if (hash_field->flags & HASH_FIELD_STACKTRACE) {
			key_parts[i].type = HASH_KEY_TYPE_STACKTRACE;
			key_parts[i].var.val_stacktrace = stacktrace;
		} else if (hash_field->flags & HASH_FIELD_STRING) {
			u64 hash_val = hash_field->fn(hash_field, rec);

			key_parts[i].type = HASH_KEY_TYPE_STRING;
			key_parts[i].var.val_string = (char *)hash_val;
		} else {
			u64 hash_val = hash_field->fn(hash_field, rec);

			key_parts[i].type = HASH_KEY_TYPE_U64;
			key_parts[i].var.val_u64 = hash_val;
		}
	}

	hlist_for_each_entry_rcu(entry, &hash_data->hashtab[hash_idx], node) {
		if (hash_trigger_entry_match(entry, key_parts, n_keys))
			return entry;
	}

	return NULL;
}

static void hash_trigger_entry_insert(struct hash_trigger_data *hash_data,
				      struct hash_trigger_entry *entry,
				      void *rec,
				      struct stack_trace *stacktrace)
{
	u64 hash_idx = hash_key(hash_data, rec, stacktrace);

	hash_data->total_entries++;

	hlist_add_head_rcu(&entry->node, &hash_data->hashtab[hash_idx]);
}

static void
hash_trigger_entry_update(struct hash_trigger_data *hash_data,
			  struct hash_trigger_entry *entry, void *rec)
{
	struct hash_field *hash_field;
	unsigned int i;
	u64 hash_val;

	for (i = 0; i < hash_data->n_vals; i++) {
		hash_field = hash_data->vals[i];
		hash_val = hash_field->fn(hash_field, rec);
		entry->sums[i] += hash_val;
	}

	entry->count++;
}

static void
early_hash_trigger_entry_update(struct hash_trigger_data *hash_data,
				struct hash_trigger_entry *entry,
				u64 *vals)
{
	struct hash_field *hash_field;
	unsigned int i;

	if (vals) {
		for (i = 0; i < hash_data->n_vals; i++) {
			hash_field = hash_data->vals[i];
			entry->sums[i] += vals[i];
		}
	}

	entry->count++;
}

static void
event_hash_trigger(struct event_trigger_data *data, void *rec)
{
	struct hash_trigger_data *hash_data = data->private_data;
	struct hash_trigger_entry *entry;
	struct hash_field *hash_field;

	struct stack_trace stacktrace;
	unsigned long entries[HASH_STACKTRACE_DEPTH];

	unsigned long flags;

	if (hash_data->drops) {
		hash_data->drops++;
		return;
	}

	hash_field = hash_data->keys[0];

	if (hash_field->flags & HASH_FIELD_STACKTRACE) {
		stacktrace.max_entries = HASH_STACKTRACE_DEPTH;
		stacktrace.entries = entries;
		stacktrace.nr_entries = 0;
		stacktrace.skip = HASH_STACKTRACE_SKIP;

		save_stack_trace(&stacktrace);
	}

	spin_lock_irqsave(&hash_data->lock, flags);
	entry = hash_trigger_entry_find(hash_data, rec, &stacktrace);

	if (!entry) {
		entry = hash_trigger_entry_create(hash_data, rec, &stacktrace);
		WARN_ON_ONCE(!entry);
		if (!entry) {
			spin_unlock_irqrestore(&hash_data->lock, flags);
			return;
		}
		hash_trigger_entry_insert(hash_data, entry, rec, &stacktrace);
	}

	hash_trigger_entry_update(hash_data, entry, rec);
	hash_data->total_hits++;
	spin_unlock_irqrestore(&hash_data->lock, flags);
}

static void
hash_trigger_stacktrace_print(struct seq_file *m,
			      struct stack_trace *stacktrace)
{
	char str[KSYM_SYMBOL_LEN];
	unsigned int spaces = 8;
	unsigned int i;

	for (i = 0; i < stacktrace->nr_entries; i++) {
		if (stacktrace->entries[i] == ULONG_MAX)
			return;
		seq_printf(m, "%*c", 1 + spaces, ' ');
		sprint_symbol(str, stacktrace->entries[i]);
		seq_printf(m, "%s\n", str);
	}
}

static void
hash_trigger_entry_print(struct seq_file *m,
			 struct hash_trigger_data *hash_data,
			 struct hash_trigger_entry *entry)
{
	char str[KSYM_SYMBOL_LEN];
	unsigned int i;

	seq_printf(m, "key: ");
	for (i = 0; i < hash_data->n_keys; i++) {
		if (i > 0)
			seq_printf(m, ", ");
		if (entry->key_parts[i].flags & HASH_FIELD_SYM) {
			kallsyms_lookup(entry->key_parts[i].var.val_u64,
					NULL, NULL, NULL, str);
			seq_printf(m, "%s:[%llx] %s",
				   hash_data->keys[i]->field->name,
				   entry->key_parts[i].var.val_u64,
				   str);
		} else if (entry->key_parts[i].flags & HASH_FIELD_HEX) {
			seq_printf(m, "%s:%llx",
				   hash_data->keys[i]->field->name,
				   entry->key_parts[i].var.val_u64);
		} else if (entry->key_parts[i].flags & HASH_FIELD_STACKTRACE) {
			seq_printf(m, "stacktrace:\n");
			hash_trigger_stacktrace_print(m,
				      entry->key_parts[i].var.val_stacktrace);
		} else if (entry->key_parts[i].flags & HASH_FIELD_STRING) {
			seq_printf(m, "%s:%s",
				   hash_data->keys[i]->field->name,
				   entry->key_parts[i].var.val_string);
		} else if (entry->key_parts[i].flags & HASH_FIELD_EXECNAME) {
			seq_printf(m, "%s:%s[%llu]",
				   hash_data->keys[i]->field->name,
				   entry->comm,
				   entry->key_parts[i].var.val_u64);
		} else if (entry->key_parts[i].flags & HASH_FIELD_SYSCALL) {
			int syscall = entry->key_parts[i].var.val_u64;
			const char *syscall_name = get_syscall_name(syscall);

			if (!syscall_name)
				syscall_name = "unknown_syscall";
			seq_printf(m, "%s:%s",
				   hash_data->keys[i]->field->name,
				   syscall_name);
		} else {
			seq_printf(m, "%s:%llu",
				   hash_data->keys[i]->field->name,
				   entry->key_parts[i].var.val_u64);
		}
	}

	seq_printf(m, "\tvals: count:%llu", entry->count);

	for (i = 0; i < hash_data->n_vals; i++) {
		if (i > 0)
			seq_printf(m, ", ");
		if (hash_data->vals[i]->aux_field) {
			seq_printf(m, " %s-%s:%llu",
				   hash_data->vals[i]->field->name,
				   hash_data->vals[i]->aux_field->name,
				   entry->sums[i]);
			continue;
		}
		seq_printf(m, " %s:%llu",
			   hash_data->vals[i]->field->name,
			   entry->sums[i]);
	}
	seq_printf(m, "\n");
}

static int sort_entries(const struct hash_trigger_entry **a,
			const struct hash_trigger_entry **b)
{
	const struct hash_trigger_entry *entry_a, *entry_b;
	struct hash_trigger_sort_key *sort_key;
	struct hash_trigger_data *hash_data;
	u64 val_a, val_b;
	int ret = 0;

	entry_a = *a;
	entry_b = *b;

	hash_data = entry_a->hash_data;
	sort_key = hash_data->sort_key_cur;

	if (sort_key->use_hitcount) {
		val_a = entry_a->count;
		val_b = entry_b->count;
	} else if (sort_key->key_part) {
		/* TODO: make sure we never use a stacktrace here */
		val_a = entry_a->key_parts[sort_key->idx].var.val_u64;
		val_b = entry_b->key_parts[sort_key->idx].var.val_u64;
	} else {
		val_a = entry_a->sums[sort_key->idx];
		val_b = entry_b->sums[sort_key->idx];
	}

	if (val_a > val_b)
		ret = 1;
	else if (val_a < val_b)
		ret = -1;

	if (sort_key->descending)
		ret = -ret;

	return ret;
}

static void sort_secondary(struct hash_trigger_data *hash_data,
			   struct hash_trigger_entry **entries,
			   unsigned int n_entries)
{
	struct hash_trigger_sort_key *primary_sort_key;
	unsigned int start = 0, n_subelts = 1;
	struct hash_trigger_entry *entry;
	bool do_sort = false;
	unsigned int i, idx;
	u64 cur_val;

	primary_sort_key = hash_data->sort_keys[0];

	entry = entries[0];
	if (primary_sort_key->use_hitcount)
		cur_val = entry->count;
	else if (primary_sort_key->key_part)
		cur_val = entry->key_parts[primary_sort_key->idx].var.val_u64;
	else
		cur_val = entry->sums[primary_sort_key->idx];

	hash_data->sort_key_cur = hash_data->sort_keys[1];

	for (i = 1; i < n_entries; i++) {
		entry = entries[i];
		if (primary_sort_key->use_hitcount) {
			if (entry->count != cur_val) {
				cur_val = entry->count;
				do_sort = true;
			}
		} else if (primary_sort_key->key_part) {
			idx = primary_sort_key->idx;
			if (entry->key_parts[idx].var.val_u64 != cur_val) {
				cur_val = entry->key_parts[idx].var.val_u64;
				do_sort = true;
			}
		} else {
			idx = primary_sort_key->idx;
			if (entry->sums[idx] != cur_val) {
				cur_val = entry->sums[idx];
				do_sort = true;
			}
		}

		if (i == n_entries - 1)
			do_sort = true;

		if (do_sort) {
			if (n_subelts > 1) {
				sort(entries + start, n_subelts, sizeof(entry),
				     (int (*)(const void *, const void *))sort_entries, NULL);
			}
			start = i;
			n_subelts = 1;
			do_sort = false;
		} else
			n_subelts++;
	}
}

static bool
print_entries_sorted(struct seq_file *m, struct hash_trigger_data *hash_data)
{
	unsigned int hashtab_size = (1 << hash_data->hashtab_bits);
	struct hash_trigger_entry **entries;
	struct hash_trigger_entry *entry;
	unsigned int entries_size;
	unsigned int i = 0, j = 0;

	entries_size = sizeof(entry) * hash_data->total_entries;
	entries = kmalloc(entries_size, GFP_KERNEL);
	if (!entries)
		return false;

	for (i = 0; i < hashtab_size; i++) {
		hlist_for_each_entry_rcu(entry, &hash_data->hashtab[i], node)
			entries[j++] = entry;
	}

	hash_data->sort_key_cur = hash_data->sort_keys[0];
	sort(entries, j, sizeof(struct hash_trigger_entry *),
	     (int (*)(const void *, const void *))sort_entries, NULL);

	if (hash_data->sort_keys[1])
		sort_secondary(hash_data, entries, j);

	for (i = 0; i < j; i++)
		hash_trigger_entry_print(m, hash_data, entries[i]);

	kfree(entries);

	return true;
}

static bool
print_entries_unsorted(struct seq_file *m, struct hash_trigger_data *hash_data)
{
	unsigned int hashtab_size = (1 << hash_data->hashtab_bits);
	struct hash_trigger_entry *entry;
	unsigned int i = 0;

	for (i = 0; i < hashtab_size; i++) {
		hlist_for_each_entry_rcu(entry, &hash_data->hashtab[i], node)
			hash_trigger_entry_print(m, hash_data, entry);
	}

	return true;
}

#define EARLY_HASHTRIGGERS_MAX	8

struct early_hashtrigger {
	char				event_name[MAX_EVENT_NAME_LEN];
	struct hash_trigger_data	*hash_data;
	bool				enabled;
};

static struct early_hashtrigger early_hashtriggers[EARLY_HASHTRIGGERS_MAX];
static unsigned int n_early_hashtriggers;

struct early_hashtrigger *find_early_hashtrigger(const char *event_name)
{
	unsigned int i;

	for (i = 0; i < EARLY_HASHTRIGGERS_MAX; i++) {
		if (!strlen(early_hashtriggers[i].event_name))
			break;
		if (!strcmp(early_hashtriggers[i].event_name, event_name))
			return &early_hashtriggers[i];
	}

	return NULL;
}

void disable_early_hashtrigger(struct ftrace_event_file *file)
{
	struct early_hashtrigger *early_hashtrigger;
	char event_name[MAX_EVENT_NAME_LEN];

	sprintf(event_name, "%s:%s", file->event_call->class->system,
		file->event_call->name);
	early_hashtrigger = find_early_hashtrigger(event_name);
	if (!early_hashtrigger)
		return;

	early_hashtrigger->enabled = false;
}

static int
event_hash_trigger_print(struct seq_file *m, struct event_trigger_ops *ops,
			 struct event_trigger_data *data)
{
	struct hash_trigger_data *hash_data = data->private_data;
	bool sorted;
	int ret;

	ret = event_trigger_print("hash", m, (void *)data->count,
				  data->filter_str);

	if (strlen(hash_data->early_event_name)) {
		struct early_hashtrigger *early_hashtrigger;
		struct hash_trigger_data *early_hash_data;

		early_hashtrigger = find_early_hashtrigger(hash_data->early_event_name);
		if (early_hashtrigger) {
			early_hash_data = early_hashtrigger->hash_data;
			if (early_hash_data) {
				seq_printf(m, "Early %s events:\n",
					   early_hash_data->early_event_name);

				sorted = print_entries_sorted(m, early_hash_data);
				if (!sorted)
					print_entries_unsorted(m, early_hash_data);

				seq_printf(m, "Totals:\n    Hits: %lu\n    Entries: %lu\n    Dropped: %lu\n",
					   early_hash_data->total_hits, early_hash_data->total_entries,
					   early_hash_data->drops);

				if (!sorted)
					seq_printf(m, "Unsorted (couldn't alloc memory for sorting)\n");
			}
		}
	}

	sorted = print_entries_sorted(m, hash_data);
	if (!sorted)
		print_entries_unsorted(m, hash_data);

	seq_printf(m, "Totals:\n    Hits: %lu\n    Entries: %lu\n    Dropped: %lu\n",
		   hash_data->total_hits, hash_data->total_entries, hash_data->drops);

	if (!sorted)
		seq_printf(m, "Unsorted (couldn't alloc memory for sorting)\n");

	return ret;
}

static void
event_hash_trigger_free(struct event_trigger_ops *ops,
			struct event_trigger_data *data)
{
	struct hash_trigger_data *hash_data = data->private_data;

	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref) {
		/* This won't ever be called for boot triggers */
		destroy_hashdata(hash_data, hash_data_kfree);
		trigger_data_free(data);
	}
}

static struct event_trigger_ops event_hash_trigger_ops = {
	.func			= event_hash_trigger,
	.print			= event_hash_trigger_print,
	.init			= event_trigger_init,
	.free			= event_hash_trigger_free,
};

static struct event_trigger_ops *
event_hash_get_trigger_ops(char *cmd, char *param)
{
	/* counts don't make sense for hash triggers */
	return &event_hash_trigger_ops;
}

static int
event_hash_trigger_func(struct event_command *cmd_ops,
			struct ftrace_event_file *file,
			char *glob, char *cmd, char *param)
{
	struct event_trigger_data *trigger_data;
	struct event_trigger_ops *trigger_ops;
	struct hash_trigger_data *hash_data;
	char *sort_keys = NULL;
	char *trigger;
	char *number;
	int ret = 0;
	char *keys;
	char *vals;

	if (!param)
		return -EINVAL;

	/* separate the trigger from the filter (s:e:n [if filter]) */
	trigger = strsep(&param, " \t");
	if (!trigger)
		return -EINVAL;

	keys = strsep(&trigger, ":");
	if (!trigger)
		return -EINVAL;

	vals = strsep(&trigger, ":");
	if (trigger)
		sort_keys = strsep(&trigger, ":");

	hash_data = create_hash_data(HASH_TRIGGER_BITS, keys, vals, sort_keys,
				     file, hash_data_kzalloc,
				     hash_data_kfree,
				     hash_data_kstrdup,
				     &ret);
	sprintf(hash_data->early_event_name, "%s:%s", file->event_call->class->system,
		file->event_call->name);

	if (ret)
		return ret;

	trigger_ops = cmd_ops->get_trigger_ops(cmd, trigger);

	ret = -ENOMEM;
	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data)
		goto out;

	trigger_data->count = -1;
	trigger_data->ops = trigger_ops;
	trigger_data->cmd_ops = cmd_ops;
	INIT_LIST_HEAD(&trigger_data->list);
	RCU_INIT_POINTER(trigger_data->filter, NULL);

	trigger_data->private_data = hash_data;

	if (glob[0] == '!') {
		cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
		ret = 0;
		goto out_free;
	}

	if (trigger) {
		number = strsep(&trigger, ":");

		ret = -EINVAL;
		if (strlen(number)) /* hash triggers don't support counts */
			goto out_free;
	}

	if (!param) /* if param is non-empty, it's supposed to be a filter */
		goto out_reg;

	if (!cmd_ops->set_filter)
		goto out_reg;

	ret = cmd_ops->set_filter(param, trigger_data, file);
	if (ret < 0)
		goto out_free;

 out_reg:
	disable_early_hashtrigger(file);
	ret = cmd_ops->reg(glob, trigger_ops, trigger_data, file);
	/*
	 * The above returns on success the # of functions enabled,
	 * but if it didn't find any functions it returns zero.
	 * Consider no functions a failure too.
	 */
	if (!ret) {
		ret = -ENOENT;
		goto out_free;
	} else if (ret < 0)
		goto out_free;
	/* Just return zero, not the number of enabled functions */
	ret = 0;
 out:
	return ret;

 out_free:
	if (cmd_ops->set_filter)
		cmd_ops->set_filter(NULL, trigger_data, NULL);
	kfree(trigger_data);
	/* This won't ever be called for boot triggers */
	destroy_hashdata(hash_data, hash_data_kfree);
	goto out;
}

static struct event_command trigger_hash_cmd= {
	.name			= "hash",
	.trigger_type		= ETT_EVENT_HASH,
	.post_trigger		= true, /* need non-NULL rec */
	.func			= event_hash_trigger_func,
	.reg			= register_trigger,
	.unreg			= unregister_trigger,
	.get_trigger_ops	= event_hash_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init int register_trigger_hash_cmd(void)
{
	int ret;

	ret = register_event_command(&trigger_hash_cmd);
	WARN_ON(ret < 0);

	return ret;
}

__init int register_trigger_cmds(void)
{
	register_trigger_traceon_traceoff_cmds();
	register_trigger_snapshot_cmd();
	register_trigger_stacktrace_cmd();
	register_trigger_enable_disable_cmds();
	register_trigger_hash_cmd();

	return 0;
}

static char early_hashtriggers_buf[COMMAND_LINE_SIZE] __initdata;

static void event_early_hash_trigger(struct hash_trigger_data *hash_data,
				     u64 *vals)
{
	struct hash_trigger_entry *entry;
	struct hash_field *hash_field;

	struct stack_trace stacktrace;
	unsigned long entries[HASH_STACKTRACE_DEPTH];

	unsigned long flags;

	if (hash_data->drops) {
		hash_data->drops++;
		return;
	}

	hash_field = hash_data->keys[0];

	if (hash_field->flags & HASH_FIELD_STACKTRACE) {
		stacktrace.max_entries = HASH_STACKTRACE_DEPTH;
		stacktrace.entries = entries;
		stacktrace.nr_entries = 0;
		stacktrace.skip = HASH_STACKTRACE_SKIP;

		save_stack_trace(&stacktrace);
	}

	spin_lock_irqsave(&hash_data->lock, flags);
	entry = hash_trigger_entry_find(hash_data, NULL, &stacktrace);
	spin_unlock_irqrestore(&hash_data->lock, flags);

	if (!entry) {
		entry = hash_trigger_entry_create(hash_data, NULL, &stacktrace);
		WARN_ON_ONCE(!entry);
		if (!entry) {
			spin_unlock_irqrestore(&hash_data->lock, flags);
			return;
		}
		spin_lock_irqsave(&hash_data->lock, flags);
		hash_trigger_entry_insert(hash_data, entry, NULL, &stacktrace);
		spin_unlock_irqrestore(&hash_data->lock, flags);
	}

	spin_lock_irqsave(&hash_data->lock, flags);
	early_hash_trigger_entry_update(hash_data, entry, vals);
	hash_data->total_hits++;
	spin_unlock_irqrestore(&hash_data->lock, flags);
}

/* per-event hacks */

static inline struct hash_trigger_data *early_event_enabled(const char *event_name)
{
	struct early_hashtrigger *early_hashtrigger;
	struct hash_trigger_data *hash_data;

	early_hashtrigger = find_early_hashtrigger(event_name);
	if (!early_hashtrigger)
		return NULL;

	if (!early_hashtrigger->enabled)
		return NULL;

	hash_data = early_hashtrigger->hash_data;
	if (!hash_data)
		return NULL;

	return hash_data;
}

void early_trace_kmalloc(unsigned long call_site, const void *ptr,
			 size_t bytes_req, size_t bytes_alloc, gfp_t gfp_flags)
{
	struct hash_trigger_data *hash_data;
	u64 vals[2];

	vals[0] = bytes_req;
	vals[1] = bytes_alloc;

	hash_data = early_event_enabled("kmem:kmalloc");
	if (hash_data)
		event_early_hash_trigger(hash_data, vals);
}
EXPORT_SYMBOL_GPL(early_trace_kmalloc);

void early_trace_kmem_cache_alloc(unsigned long call_site, const void *ptr,
				  size_t bytes_req, size_t bytes_alloc, gfp_t gfp_flags)
{
	struct hash_trigger_data *hash_data;
	u64 vals[2];

	vals[0] = bytes_req;
	vals[1] = bytes_alloc;

	hash_data = early_event_enabled("kmem:kmem_cache_alloc");
	if (hash_data)
		event_early_hash_trigger(hash_data, vals);
}
EXPORT_SYMBOL_GPL(early_trace_kmem_cache_alloc);

void early_trace_kmalloc_node(unsigned long call_site, const void *ptr,
			      size_t bytes_req, size_t bytes_alloc,
			      gfp_t gfp_flags, int node)
{
	struct hash_trigger_data *hash_data;
	u64 vals[2];

	vals[0] = bytes_req;
	vals[1] = bytes_alloc;

	hash_data = early_event_enabled("kmem:kmalloc_node");
	if (hash_data)
		event_early_hash_trigger(hash_data, vals);
}
EXPORT_SYMBOL_GPL(early_trace_kmalloc_node);

void early_trace_kmem_cache_alloc_node(unsigned long call_site, const void *ptr,
				       size_t bytes_req, size_t bytes_alloc,
				       gfp_t gfp_flags, int node)
{
	struct hash_trigger_data *hash_data;
	u64 vals[2];

	vals[0] = bytes_req;
	vals[1] = bytes_alloc;

	hash_data = early_event_enabled("kmem:kmem_cache_alloc_node");
	if (hash_data)
		event_early_hash_trigger(hash_data, vals);
}
EXPORT_SYMBOL_GPL(early_trace_kmem_cache_alloc_node);


void early_trace_mm_page_alloc(struct page *page, unsigned int order,
			       gfp_t gfp_flags, int migratetype)
{
	struct hash_trigger_data *hash_data;

	hash_data = early_event_enabled("kmem:mm_page_alloc");
	if (hash_data)
		event_early_hash_trigger(hash_data, NULL);
}
EXPORT_SYMBOL_GPL(early_trace_mm_page_alloc);

void early_trace_mm_page_alloc_extfrag(struct page *page,
				       int alloc_order, int fallback_order,
				       int alloc_migratetype, int fallback_migratetype, int new_migratetype)
{
	struct hash_trigger_data *hash_data;

	hash_data = early_event_enabled("kmem:mm_page_alloc_extfrag");
	if (hash_data)
		event_early_hash_trigger(hash_data, NULL);
}
EXPORT_SYMBOL_GPL(early_trace_mm_page_alloc_extfrag);

void early_trace_mm_page_alloc_zone_locked(struct page *page,
					   unsigned int order, int migratetype)
{
	struct hash_trigger_data *hash_data;

	hash_data = early_event_enabled("kmem:mm_page_alloc_zone_locked");
	if (hash_data)
		event_early_hash_trigger(hash_data, NULL);
}
EXPORT_SYMBOL_GPL(early_trace_mm_page_alloc_zone_locked);

/*
 * For now, we only allow subsys:event:hash:stacktrace:hitcount, which
 * allows us to use NULL event_files.  The source will manually do
 * what it wants.
 */
static __init int setup_early_hashtrigger(char *hashtrigger_str)
{
	struct hash_trigger_data *hash_data;
	char *sort_keys = NULL;
	char *trigger;
	char *subsys, *event;
	char *hash;
	char *vals;
	char *keys;
	int ret = 0;

	if (n_early_hashtriggers == EARLY_HASHTRIGGERS_MAX)
		return -EINVAL;

	/* separate the trigger from the filter (s:e:n [if filter]) */
	trigger = strsep(&hashtrigger_str, " \t");
	if (!trigger)
		return -EINVAL;

	subsys = strsep(&trigger, ":");
	if (!subsys || !trigger)
		return -EINVAL;

	event = strsep(&trigger, ":");
	if (!event || !trigger)
		return -EINVAL;

	hash = strsep(&trigger, ":");
	if (!hash || !trigger)
		return -EINVAL;

	keys = strsep(&trigger, ":");
	if (!keys || !trigger)
		return -EINVAL;

	vals = strsep(&trigger, ":");
	if (!vals) // zzzz for normal case too?
		return -EINVAL;

	if (trigger) {
		sort_keys = strsep(&trigger, ":");
		if (!sort_keys) // zzzz for normal case too?
			return -EINVAL;
	}

	hash_data = create_hash_data(12 /* 2048 * 2 */, keys, vals, sort_keys,
				     NULL, hash_data_bootmem_alloc,
				     hash_data_bootmem_free,
				     hash_data_bootmem_strdup,
				     &ret);
	sprintf(hash_data->early_event_name, "%s:%s", subsys, event);

	if (!hash_data)
		return -EINVAL;

	sprintf(early_hashtriggers[n_early_hashtriggers].event_name, "%s:%s",
		subsys, event);

	early_hashtriggers[n_early_hashtriggers].hash_data = hash_data;
	early_hashtriggers[n_early_hashtriggers++].enabled = true;

	return ret;
}

static __init int setup_early_hashtriggers(char *str)
{
	char *hashtrigger_str, *hashtrigger_strings;
	int ret = 0;

	strlcpy(early_hashtriggers_buf, str, COMMAND_LINE_SIZE);
	hashtrigger_strings = early_hashtriggers_buf;

	/* Use semicolon as hashtrigger separator.  We already use ',:=-' */
	while ((hashtrigger_str = strsep(&hashtrigger_strings, ";"))) {
		ret = setup_early_hashtrigger(hashtrigger_str);
		if (ret)
			break;
	}

	return ret;
}
early_param("trace_event_hashtriggers", setup_early_hashtriggers);
