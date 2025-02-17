/*
 * drivers/cpufreq/cpufreq_lulzactive.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Author: Mike Chan (mike@android.com)
 * Edited: Tegrak (luciferanna@gmail.com)
 *
 * Driver values in /sys/devices/system/cpu/cpufreq/lulzactive
 * 
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/earlysuspend.h>
#include <asm/cputime.h>
#include <linux/suspend.h>

#define LOGI(fmt...) printk(KERN_INFO "[lulzactive] " fmt)
#define LOGW(fmt...) printk(KERN_WARNING "[lulzactive] " fmt)
#define LOGD(fmt...) printk(KERN_DEBUG "[lulzactive] " fmt)

static void (*pm_idle_old)(void);
static atomic_t active_count = ATOMIC_INIT(0);

struct cpufreq_lulzactive_cpuinfo {
	struct timer_list cpu_timer;
	int timer_idlecancel;
	u64 time_in_idle;
	u64 idle_exit_time;
	u64 timer_run_time;
	int idling;
	u64 freq_change_time;
	u64 freq_change_time_in_idle;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	unsigned int target_freq;
	int governor_enabled;
};

static DEFINE_PER_CPU(struct cpufreq_lulzactive_cpuinfo, cpuinfo);

/* Workqueues handle frequency scaling */
static struct task_struct *up_task;
static struct workqueue_struct *down_wq;
static struct work_struct freq_scale_down_work;
static cpumask_t up_cpumask;
static spinlock_t up_cpumask_lock;
static cpumask_t down_cpumask;
static spinlock_t down_cpumask_lock;

/*
 * The minimum amount of time to spend at a frequency before we can ramp up.
 */
#define DEFAULT_UP_SAMPLE_TIME 24000
static unsigned long up_sample_time;

/*
 * The minimum amount of time to spend at a frequency before we can ramp down.
 */
#define DEFAULT_DOWN_SAMPLE_TIME 49000 //80000;
static unsigned long down_sample_time;

/*
 * DEBUG print flags
 */
#define DEFAULT_DEBUG_MODE (11)
static unsigned long debug_mode;
enum {
	LULZACTIVE_DEBUG_EARLY_SUSPEND=1,
	LULZACTIVE_DEBUG_START_STOP=2,
	LULZACTIVE_DEBUG_LOAD=4,
	LULZACTIVE_DEBUG_SUSPEND=8,
};

/*
 * CPU freq will be increased if measured load > inc_cpu_load;
 */
#define DEFAULT_INC_CPU_LOAD 60
static unsigned long inc_cpu_load;

/*
 * CPU freq will be decreased if measured load < dec_cpu_load;
 */
#define DEFAULT_DEC_CPU_LOAD 30
static unsigned long dec_cpu_load;

/*
 * Increasing frequency table index
 * zero disables and causes to always jump straight to max frequency.
 */
#define DEFAULT_RAMP_UP_STEP 1
static unsigned long ramp_up_step;

/*
 * Decreasing frequency table index
 * zero disables and will calculate frequency according to load heuristic.
 */
#define DEFAULT_RAMP_DOWN_STEP 1
static unsigned long ramp_down_step;

/*
 * Use minimum frequency while suspended.
 */
static unsigned int suspending;
#define DEFAULT_SUSPENDING_MIN_FREQ 320000
static unsigned int suspending_min_freq;
static unsigned int early_suspended;

#define DEBUG 0
#define BUFSZ 128

#if DEBUG
#include <linux/proc_fs.h>

struct dbgln {
	int cpu;
	unsigned long jiffy;
	unsigned long run;
	char buf[BUFSZ];
};

#define NDBGLNS 256

static struct dbgln dbgbuf[NDBGLNS];
static int dbgbufs;
static int dbgbufe;
static struct proc_dir_entry	*dbg_proc;
static spinlock_t dbgpr_lock;

static u64 up_request_time;
static unsigned int up_max_latency;

static void dbgpr(char *fmt, ...)
{
	va_list args;
	int n;
	unsigned long flags;

	spin_lock_irqsave(&dbgpr_lock, flags);
	n = dbgbufe;
        va_start(args, fmt);
        vsnprintf(dbgbuf[n].buf, BUFSZ, fmt, args);
        va_end(args);
	dbgbuf[n].cpu = smp_processor_id();
	dbgbuf[n].run = nr_running();
	dbgbuf[n].jiffy = jiffies;

	if (++dbgbufe >= NDBGLNS)
		dbgbufe = 0;

	if (dbgbufe == dbgbufs)
		if (++dbgbufs >= NDBGLNS)
			dbgbufs = 0;

	spin_unlock_irqrestore(&dbgpr_lock, flags);
}

static void dbgdump(void)
{
	int i, j;
	unsigned long flags;
	static struct dbgln prbuf[NDBGLNS];

	spin_lock_irqsave(&dbgpr_lock, flags);
	i = dbgbufs;
	j = dbgbufe;
	memcpy(prbuf, dbgbuf, sizeof(dbgbuf));
	dbgbufs = 0;
	dbgbufe = 0;
	spin_unlock_irqrestore(&dbgpr_lock, flags);

	while (i != j)
	{
		printk("%lu %d %lu %s",
		       prbuf[i].jiffy, prbuf[i].cpu, prbuf[i].run,
		       prbuf[i].buf);
		if (++i == NDBGLNS)
			i = 0;
	}
}

static int dbg_proc_read(char *buffer, char **start, off_t offset,
			       int count, int *peof, void *dat)
{
	printk("max up_task latency=%uus\n", up_max_latency);
	dbgdump();
	*peof = 1;
	return 0;
}


#else
#define dbgpr(...) do {} while (0)
#endif

static int cpufreq_governor_lulzactive(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_LULZACTIVE
static
#endif
struct cpufreq_governor cpufreq_gov_lulzactive = {
	.name = "lulzactive",
	.governor = cpufreq_governor_lulzactive,
	.max_transition_latency = 9000000,
	.owner = THIS_MODULE,
};

static void cpufreq_lulzactive_timer(unsigned long data)
{
	unsigned int delta_idle;
	unsigned int delta_time;
	int cpu_load;
	int load_since_change;
	u64 time_in_idle;
	u64 idle_exit_time;
	struct cpufreq_lulzactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	u64 now_idle;
	unsigned int new_freq;
	unsigned int index, index_old;
	int ret;
	static unsigned int stuck_on_sampling = 0;
	static unsigned int cpu_load_captured = 0;

	/*
	 * Once pcpu->timer_run_time is updated to >= pcpu->idle_exit_time,
	 * this lets idle exit know the current idle time sample has
	 * been processed, and idle exit can generate a new sample and
	 * re-arm the timer.  This prevents a concurrent idle
	 * exit on that CPU from writing a new set of info at the same time
	 * the timer function runs (the timer function can't use that info
	 * until more time passes).
	 */
	time_in_idle = pcpu->time_in_idle;
	idle_exit_time = pcpu->idle_exit_time;
	now_idle = get_cpu_idle_time_us(data, &pcpu->timer_run_time);
	smp_wmb();

	/* If we raced with cancelling a timer, skip. */
	if (!idle_exit_time) {
		dbgpr("timer %d: no valid idle exit sample\n", (int) data);
		goto exit;
	}

	/* let it be when s5pv310 contorl the suspending by tegrak */
	//if (suspending) {
	//	goto rearm;
	//}

#if DEBUG
	if ((int) jiffies - (int) pcpu->cpu_timer.expires >= 10)
		dbgpr("timer %d: late by %d ticks\n",
		      (int) data, jiffies - pcpu->cpu_timer.expires);
#endif

	delta_idle = (unsigned int) cputime64_sub(now_idle, time_in_idle);
	delta_time = (unsigned int) cputime64_sub(pcpu->timer_run_time,
						  idle_exit_time);

	/*
	 * If timer ran less than 1ms after short-term sample started, retry.
	 */
	if (delta_time < 1000) {
		dbgpr("timer %d: time delta %u too short exit=%llu now=%llu\n", (int) data,
		      delta_time, idle_exit_time, pcpu->timer_run_time);
		goto rearm;
	}

	if (delta_idle > delta_time)
		cpu_load = 0;
	else
		cpu_load = 100 * (delta_time - delta_idle) / delta_time;

	delta_idle = (unsigned int) cputime64_sub(now_idle,
						 pcpu->freq_change_time_in_idle);
	delta_time = (unsigned int) cputime64_sub(pcpu->timer_run_time,
						  pcpu->freq_change_time);

	if (delta_idle > delta_time)
		load_since_change = 0;
	else
		load_since_change =
			100 * (delta_time - delta_idle) / delta_time;

	/*
	 * Choose greater of short-term load (since last idle timer
	 * started or timer function re-armed itself) or long-term load
	 * (since last frequency change).
	 */
	if (load_since_change > cpu_load)
		cpu_load = load_since_change;

	/* 
	 * START lulzactive algorithm section
	 */	
	/*
	if (suspending) {
		new_freq = pcpu->policy->cur;
	}
	else */
	if (early_suspended) {
		new_freq = pcpu->policy->min;
		pcpu->target_freq = pcpu->policy->cur;
	}
	else if (cpu_load >= inc_cpu_load) {
		if (ramp_up_step && pcpu->policy->cur < pcpu->policy->max) {
			ret = cpufreq_frequency_table_target(
				pcpu->policy, pcpu->freq_table,
				pcpu->policy->cur, CPUFREQ_RELATION_H,
				&index);
			if (ret < 0) {
				goto rearm;
			}		
			//set next high frequency of table
			if (index > 0) 
				--index;
			new_freq = pcpu->freq_table[index].frequency;
		}
		else {
			new_freq = pcpu->policy->max;
		}
	}
	/*
	else if (cpu_load <= dec_cpu_load) {
		ret = cpufreq_frequency_table_target(
			pcpu->policy, pcpu->freq_table,
			pcpu->policy->cur, CPUFREQ_RELATION_H,
			&index);
		if (ret < 0) {
			goto rearm;
		}
		if (ramp_down_step) {
			//set next low frequency of table
			new_freq = pcpu->freq_table[index + 1].frequency;
		}
		else if (ramp_down_step) {
			//new_freq = pcpu->policy->max * cpu_load / 100;
			new_freq = pcpu->policy->min;
		}
	}
	*/
	else if (stuck_on_sampling) {
		new_freq = pcpu->policy->cur;
	}
	else {		
		if (ramp_down_step) {
			ret = cpufreq_frequency_table_target(
				pcpu->policy, pcpu->freq_table,
				pcpu->policy->cur, CPUFREQ_RELATION_H,
				&index);
			if (ret < 0) {
				goto rearm;
			}
			if (pcpu->freq_table[index + 1].frequency != CPUFREQ_TABLE_END) {
				++index;
			}
			new_freq = (pcpu->policy->cur > pcpu->policy->min) ? 
				(pcpu->freq_table[index].frequency) :
				(pcpu->policy->min);
		}
		else {
			new_freq = pcpu->policy->max * cpu_load / 100;
			ret = cpufreq_frequency_table_target(
				pcpu->policy, pcpu->freq_table,
				new_freq, CPUFREQ_RELATION_H,
				&index);
			if (ret < 0) {
				goto rearm;
			}
			new_freq = pcpu->freq_table[index].frequency;
		}		
	}

	if (pcpu->target_freq == new_freq)
	{
		dbgpr("timer %d: load=%d, already at %d\n", (int) data, cpu_load, new_freq);
		stuck_on_sampling = 0;
		cpu_load_captured = 0;
		goto rearm_if_notmax;
	}

	/*
	 * Do not scale down unless we have been at this frequency for the
	 * minimum sample time.
	 */
	if (new_freq < pcpu->target_freq) {
		if (cputime64_sub(pcpu->timer_run_time, pcpu->freq_change_time) <
		    down_sample_time) {
			dbgpr("timer %d: load=%d cur=%d tgt=%d not yet\n", (int) data, cpu_load, pcpu->target_freq, new_freq);
			goto rearm;
		}
	}
	else {
		if (cputime64_sub(pcpu->timer_run_time, pcpu->freq_change_time) <
		    up_sample_time) {
			dbgpr("timer %d: load=%d cur=%d tgt=%d not yet\n", (int) data, cpu_load, pcpu->target_freq, new_freq);
			/* don't reset timer */
			stuck_on_sampling = 1;
			goto rearm;
		}
	}

	if (suspending && debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
		LOGI("suspending: cpu_load=%d%% new_freq=%u ppcpu->policy->cur=%u\n", 
			 cpu_load, new_freq, pcpu->policy->cur);
	}
	if (early_suspended && !suspending && debug_mode & LULZACTIVE_DEBUG_EARLY_SUSPEND) {		
		LOGI("early_suspended: cpu_load=%d%% new_freq=%u ppcpu->policy->cur=%u\n", 
			 cpu_load, new_freq, pcpu->policy->cur);
		LOGI("lock @%uMHz!\n", new_freq/1000);
	}
	if (debug_mode & LULZACTIVE_DEBUG_LOAD && !early_suspended && !suspending) {
		LOGI("cpu_load=%d%% new_freq=%u pcpu->target_freq=%u pcpu->policy->cur=%u\n", 
			 cpu_load, new_freq, pcpu->target_freq, pcpu->policy->cur);
	}

	dbgpr("timer %d: load=%d cur=%d tgt=%d queue\n", (int) data, cpu_load, pcpu->target_freq, new_freq);

	stuck_on_sampling = 0;
	cpu_load_captured = 0;

	if (new_freq < pcpu->target_freq) {
		pcpu->target_freq = new_freq;
		spin_lock(&down_cpumask_lock);
		cpumask_set_cpu(data, &down_cpumask);
		spin_unlock(&down_cpumask_lock);
		queue_work(down_wq, &freq_scale_down_work);
	} else {
		pcpu->target_freq = new_freq;
#if DEBUG
		up_request_time = ktime_to_us(ktime_get());
#endif
		spin_lock(&up_cpumask_lock);
		cpumask_set_cpu(data, &up_cpumask);
		spin_unlock(&up_cpumask_lock);
		wake_up_process(up_task);
	}

rearm_if_notmax:
	/*
	 * Already set max speed and don't see a need to change that,
	 * wait until next idle to re-evaluate, don't need timer.
	 */
	if (pcpu->target_freq == pcpu->policy->max)
		goto exit;

rearm:
	if (!timer_pending(&pcpu->cpu_timer)) {
		/*
		 * If already at min: if that CPU is idle, don't set timer.
		 * Else cancel the timer if that CPU goes idle.  We don't
		 * need to re-evaluate speed until the next idle exit.
		 */
		if (pcpu->target_freq == pcpu->policy->min) {
			smp_rmb();

			if (pcpu->idling) {
				dbgpr("timer %d: cpu idle, don't re-arm\n", (int) data);
				goto exit;
			}

			pcpu->timer_idlecancel = 1;
		}

		pcpu->time_in_idle = get_cpu_idle_time_us(
			data, &pcpu->idle_exit_time);
		mod_timer(&pcpu->cpu_timer, jiffies + 2);
		dbgpr("timer %d: set timer for %lu exit=%llu\n", (int) data, pcpu->cpu_timer.expires, pcpu->idle_exit_time);
	}

exit:
	return;
}

static void cpufreq_lulzactive_idle(void)
{
	struct cpufreq_lulzactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());
	int pending;

	if (!pcpu->governor_enabled) {
		pm_idle_old();
		return;
	}

	pcpu->idling = 1;
	smp_wmb();
	pending = timer_pending(&pcpu->cpu_timer);

	if (pcpu->target_freq != pcpu->policy->min) {
#ifdef CONFIG_SMP
		/*
		 * Entering idle while not at lowest speed.  On some
		 * platforms this can hold the other CPU(s) at that speed
		 * even though the CPU is idle. Set a timer to re-evaluate
		 * speed so this idle CPU doesn't hold the other CPUs above
		 * min indefinitely.  This should probably be a quirk of
		 * the CPUFreq driver.
		 */
		if (!pending) {
			pcpu->time_in_idle = get_cpu_idle_time_us(
				smp_processor_id(), &pcpu->idle_exit_time);
			pcpu->timer_idlecancel = 0;
			mod_timer(&pcpu->cpu_timer, jiffies + 2);
			dbgpr("idle: enter at %d, set timer for %lu exit=%llu\n",
			      pcpu->target_freq, pcpu->cpu_timer.expires,
			      pcpu->idle_exit_time);
		}
#endif
	} else {
		/*
		 * If at min speed and entering idle after load has
		 * already been evaluated, and a timer has been set just in
		 * case the CPU suddenly goes busy, cancel that timer.  The
		 * CPU didn't go busy; we'll recheck things upon idle exit.
		 */
		if (pending && pcpu->timer_idlecancel) {
			dbgpr("idle: cancel timer for %lu\n", pcpu->cpu_timer.expires);
			del_timer(&pcpu->cpu_timer);
			/*
			 * Ensure last timer run time is after current idle
			 * sample start time, so next idle exit will always
			 * start a new idle sampling period.
			 */
			pcpu->idle_exit_time = 0;
			pcpu->timer_idlecancel = 0;
		}
	}

	pm_idle_old();
	pcpu->idling = 0;
	smp_wmb();

	/*
	 * Arm the timer for 1-2 ticks later if not already, and if the timer
	 * function has already processed the previous load sampling
	 * interval.  (If the timer is not pending but has not processed
	 * the previous interval, it is probably racing with us on another
	 * CPU.  Let it compute load based on the previous sample and then
	 * re-arm the timer for another interval when it's done, rather
	 * than updating the interval start time to be "now", which doesn't
	 * give the timer function enough time to make a decision on this
	 * run.)
	 */
	if (timer_pending(&pcpu->cpu_timer) == 0 &&
	    pcpu->timer_run_time >= pcpu->idle_exit_time) {
		pcpu->time_in_idle =
			get_cpu_idle_time_us(smp_processor_id(),
					     &pcpu->idle_exit_time);
		pcpu->timer_idlecancel = 0;
		mod_timer(&pcpu->cpu_timer, jiffies + 2);
		dbgpr("idle: exit, set timer for %lu exit=%llu\n", pcpu->cpu_timer.expires, pcpu->idle_exit_time);
#if DEBUG
	} else if (timer_pending(&pcpu->cpu_timer) == 0 &&
		   pcpu->timer_run_time < pcpu->idle_exit_time) {
		dbgpr("idle: timer not run yet: exit=%llu tmrrun=%llu\n",
		      pcpu->idle_exit_time, pcpu->timer_run_time);
#endif
	}

}

static int cpufreq_lulzactive_up_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	struct cpufreq_lulzactive_cpuinfo *pcpu;

#if DEBUG
	u64 now;
	u64 then;
	unsigned int lat;
#endif

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock(&up_cpumask_lock);

		if (cpumask_empty(&up_cpumask)) {
			spin_unlock(&up_cpumask_lock);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock(&up_cpumask_lock);
		}

		set_current_state(TASK_RUNNING);

#if DEBUG
		then = up_request_time;
		now = ktime_to_us(ktime_get());

		if (now > then) {
			lat = ktime_to_us(ktime_get()) - then;

			if (lat > up_max_latency)
				up_max_latency = lat;
		}
#endif

		tmp_mask = up_cpumask;
		cpumask_clear(&up_cpumask);
		spin_unlock(&up_cpumask_lock);

		for_each_cpu(cpu, &tmp_mask) {
			pcpu = &per_cpu(cpuinfo, cpu);

			if (nr_running() == 1) {
				dbgpr("up %d: tgt=%d nothing else running\n", cpu,
				      pcpu->target_freq);
			}

			__cpufreq_driver_target(pcpu->policy,
						pcpu->target_freq,
						CPUFREQ_RELATION_H);
			pcpu->freq_change_time_in_idle =
				get_cpu_idle_time_us(cpu,
						     &pcpu->freq_change_time);
			dbgpr("up %d: set tgt=%d (actual=%d)\n", cpu, pcpu->target_freq, pcpu->policy->cur);
		}
	}

	return 0;
}

static void cpufreq_lulzactive_freq_down(struct work_struct *work)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	struct cpufreq_lulzactive_cpuinfo *pcpu;

	spin_lock(&down_cpumask_lock);
	tmp_mask = down_cpumask;
	cpumask_clear(&down_cpumask);
	spin_unlock(&down_cpumask_lock);

	for_each_cpu(cpu, &tmp_mask) {
		pcpu = &per_cpu(cpuinfo, cpu);
		__cpufreq_driver_target(pcpu->policy,
					pcpu->target_freq,
					CPUFREQ_RELATION_H);
		pcpu->freq_change_time_in_idle =
			get_cpu_idle_time_us(cpu,
					     &pcpu->freq_change_time);
		dbgpr("down %d: set tgt=%d (actual=%d)\n", cpu, pcpu->target_freq, pcpu->policy->cur);
	}
}

static ssize_t show_down_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", down_sample_time);
}

static ssize_t store_down_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return strict_strtoul(buf, 0, &down_sample_time);
}

static struct global_attr down_sample_time_attr = __ATTR(down_sample_time, 0644,
		show_down_sample_time, store_down_sample_time);

static ssize_t show_up_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", up_sample_time);
}

static ssize_t store_up_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return strict_strtoul(buf, 0, &up_sample_time);
}

static struct global_attr up_sample_time_attr = __ATTR(up_sample_time, 0644,
		show_up_sample_time, store_up_sample_time);

static ssize_t show_debug_mode(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", debug_mode);
}

static ssize_t store_debug_mode(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return strict_strtoul(buf, 0, &debug_mode);
}

static struct global_attr debug_mode_attr = __ATTR(debug_mode, 0644,
		show_debug_mode, store_debug_mode);

static struct attribute *lulzactive_attributes[] = {
	&up_sample_time_attr.attr,
	&down_sample_time_attr.attr,
	&debug_mode_attr.attr,
	NULL,
};

static struct attribute_group lulzactive_attr_group = {
	.attrs = lulzactive_attributes,
	.name = "lulzactive",
};

static int cpufreq_governor_lulzactive(struct cpufreq_policy *new_policy,
		unsigned int event)
{
	int rc;
	struct cpufreq_lulzactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, new_policy->cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if (debug_mode & LULZACTIVE_DEBUG_START_STOP) {
			LOGI("CPUFREQ_GOV_START\n");
		}
		if (!cpu_online(new_policy->cpu))
			return -EINVAL;

		pcpu->policy = new_policy;
		pcpu->freq_table = cpufreq_frequency_get_table(new_policy->cpu);
		pcpu->target_freq = new_policy->cur;
		pcpu->freq_change_time_in_idle =
			get_cpu_idle_time_us(new_policy->cpu,
					     &pcpu->freq_change_time);
		pcpu->governor_enabled = 1;
		/*
		 * Do not register the idle hook and create sysfs
		 * entries if we have already done so.
		 */
		if (atomic_inc_return(&active_count) > 1)
			return 0;

		rc = sysfs_create_group(cpufreq_global_kobject,
				&lulzactive_attr_group);
		if (rc)
			return rc;

		pm_idle_old = pm_idle;
		pm_idle = cpufreq_lulzactive_idle;
		break;

	case CPUFREQ_GOV_STOP:
		if (debug_mode & LULZACTIVE_DEBUG_START_STOP) {
			LOGI("CPUFREQ_GOV_STOP\n");
		}
		pcpu->governor_enabled = 0;

		if (atomic_dec_return(&active_count) > 0)
			return 0;

		sysfs_remove_group(cpufreq_global_kobject,
				&lulzactive_attr_group);

		pm_idle = pm_idle_old;
		del_timer(&pcpu->cpu_timer);
		break;

	case CPUFREQ_GOV_LIMITS:
		if (new_policy->max < new_policy->cur)
			__cpufreq_driver_target(new_policy,
					new_policy->max, CPUFREQ_RELATION_H);
		else if (new_policy->min > new_policy->cur)
			__cpufreq_driver_target(new_policy,
					new_policy->min, CPUFREQ_RELATION_L);
		break;
	}
	return 0;
}

static void lulzactive_early_suspend(struct early_suspend *handler) {
	early_suspended = 1;
	if (debug_mode & LULZACTIVE_DEBUG_EARLY_SUSPEND) {
		LOGI("%s\n", __func__);
	}
}

static void lulzactive_late_resume(struct early_suspend *handler) {
	early_suspended = 0;
	if (debug_mode & LULZACTIVE_DEBUG_EARLY_SUSPEND) {
		LOGI("%s\n", __func__);
	}
}

static struct early_suspend lulzactive_power_suspend = {
	.suspend = lulzactive_early_suspend,
	.resume = lulzactive_late_resume,
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 1,
};

static int lulzactive_pm_notifier_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	struct cpufreq_policy* policy;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		suspending = 1;
		if (debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
			LOGI("PM_SUSPEND_PREPARE");
			policy = cpufreq_cpu_get(0);
			if (policy) {
				LOGI("PM_SUSPEND_PREPARE using @%uMHz\n", policy->cur);
			}
		}
		break;
	case PM_POST_SUSPEND:
		suspending = 0;
		if (debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
			LOGI("PM_POST_SUSPEND");
			policy = cpufreq_cpu_get(0);
			if (policy) {
				LOGI("PM_POST_SUSPEND using @%uMHz\n", policy->cur);
			}
		}
		break;
	case PM_RESTORE_PREPARE:
		if (debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
			LOGI("PM_RESTORE_PREPARE");
		}
		break;
	case PM_POST_RESTORE:
		if (debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
			LOGI("PM_POST_RESTORE");
		}
		break;
	case PM_HIBERNATION_PREPARE:
		if (debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
			LOGI("PM_HIBERNATION_PREPARE");
		}
		break;
	case PM_POST_HIBERNATION:
		if (debug_mode & LULZACTIVE_DEBUG_SUSPEND) {
			LOGI("PM_POST_HIBERNATION");
		}
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block lulzactive_pm_notifier = {
	.notifier_call = lulzactive_pm_notifier_event,
};

static int __init cpufreq_lulzactive_init(void)
{
	unsigned int i;
	struct cpufreq_lulzactive_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	up_sample_time = DEFAULT_UP_SAMPLE_TIME;
	down_sample_time = DEFAULT_DOWN_SAMPLE_TIME;
	debug_mode = DEFAULT_DEBUG_MODE;
	inc_cpu_load = DEFAULT_INC_CPU_LOAD;
	dec_cpu_load = DEFAULT_DEC_CPU_LOAD;
	ramp_up_step = DEFAULT_RAMP_UP_STEP;
	ramp_down_step = DEFAULT_RAMP_DOWN_STEP;
	early_suspended = 0;
	suspending = 0;
	suspending_min_freq = DEFAULT_SUSPENDING_MIN_FREQ;

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_lulzactive_timer;
		pcpu->cpu_timer.data = i;
	}

	up_task = kthread_create(cpufreq_lulzactive_up_task, NULL,
				 "klulzactiveup");
	if (IS_ERR(up_task))
		return PTR_ERR(up_task);

	sched_setscheduler_nocheck(up_task, SCHED_FIFO, &param);
	get_task_struct(up_task);

	/* No rescuer thread, bind to CPU queuing the work for possibly
	   warm cache (probably doesn't matter much). */
	down_wq = create_workqueue("klulzactive_down");

	if (! down_wq)
		goto err_freeuptask;

	INIT_WORK(&freq_scale_down_work,
		  cpufreq_lulzactive_freq_down);

#if DEBUG
	spin_lock_init(&dbgpr_lock);
	dbg_proc = create_proc_entry("igov", S_IWUSR | S_IRUGO, NULL);
	dbg_proc->read_proc = dbg_proc_read;
#endif
	spin_lock_init(&down_cpumask_lock);
	spin_lock_init(&up_cpumask_lock);

	register_pm_notifier(&lulzactive_pm_notifier);
	register_early_suspend(&lulzactive_power_suspend);

	return cpufreq_register_governor(&cpufreq_gov_lulzactive);

err_freeuptask:
	put_task_struct(up_task);
	return -ENOMEM;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_LULZACTIVE
fs_initcall(cpufreq_lulzactive_init);
#else
module_init(cpufreq_lulzactive_init);
#endif

static void __exit cpufreq_lulzactive_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_lulzactive);
	unregister_early_suspend(&lulzactive_power_suspend);
	unregister_pm_notifier(&lulzactive_pm_notifier);
	kthread_stop(up_task);
	put_task_struct(up_task);
	destroy_workqueue(down_wq);
}

module_exit(cpufreq_lulzactive_exit);

MODULE_AUTHOR("Tegrak <luciferanna@gmail.com>");
MODULE_DESCRIPTION("'lulzactive' - improved interactive governor inspired by smartass");
MODULE_LICENSE("GPL");
