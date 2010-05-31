#ifndef RM_C_
# define RM_C_

/*
    (c) Quentin Casasnovas (quentin.casasnovas@gmail.com)

    This file is part of rm.

    rm is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    rm is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with rm. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/notifier.h>
#include <linux/vmalloc.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/sched.h>

#include <asm/leon.h>

MODULE_DESCRIPTION("Ressource manager");
MODULE_AUTHOR("Quentin Casasnovas");
MODULE_LICENSE("GPL");

struct managed_cpu {
	struct list_head online_list;
	struct list_head killed_list;

	unsigned long cpu_load;
	unsigned long smoothed_load;

	cputime64_t rel_system;
	cputime64_t rel_user;
	cputime64_t rel_total;

	int cpu;
};

struct rm {
	struct task_struct *killer_thread;

	struct managed_cpu *managed_cpus;

	struct proc_dir_entry *proc_dir;
	unsigned int nr_cpus_killed;
	unsigned int qof;
	unsigned int active;
	unsigned int periodicity;
	unsigned int smooth_coef;

	unsigned int nr_running;
	unsigned int sum_smoothed_load;

	unsigned int locked;
};

#define MAX_LOAD		1000
#define HIGH_LOAD_LIMIT		(85 * MAX_LOAD / 100)


#define PIO_BADDR 0xEFFD0000

#define PER_OFFSET	0x00
#define OER_OFFSET	0x10
#define SODR_OFFSET	0x30
#define CODR_OFFSET	0x34
#define ECR_OFFSET	0x50
#define PMSR_OFFSET	0x58

struct rm rm = {
	.killer_thread = NULL,
	.managed_cpus = NULL,
	.proc_dir = NULL,
	.nr_cpus_killed = 0,
	.qof = MAX_LOAD,
	.active = 0,
	.periodicity = 250,
	.smooth_coef = 4,
	.nr_running = 0,
	.sum_smoothed_load = MAX_LOAD / 2,
	.locked = 0
};

/**
 * Smooth the load of a cpu. This permits to be insensible to small tasks that
 * heavely loads the cpu for short time : it would not be necessary to react on
 * those short tasks : by the time we would wake up a cpu, the task would be
 * finished.
 */
static void rm_update_smoothed_load(struct managed_cpu *cpu)
{
	unsigned int delta =
		max(cpu->smoothed_load, cpu->cpu_load) -
		min(cpu->smoothed_load, cpu->cpu_load);
	unsigned int step = delta / (rm.smooth_coef);

	if (cpu->smoothed_load < cpu->cpu_load)
		cpu->smoothed_load += step;
	else {
		if (step < cpu->smoothed_load)
			cpu->smoothed_load -= step;
		else
			cpu->smoothed_load = 0;
	}
}

/**
 * Updates information about each managed cpu and calculate their load in
 * percentage. This load is not smoothed and represent the average load since
 * the last call to this function.
 */
static unsigned int rm_update_cpus_load(void)
{
	struct managed_cpu *managed_cpu;
	struct cpu_usage_stat *cpustat;
	struct list_head *pos, *q;
	unsigned int nr_cpus = 0;
	cputime64_t now = jiffies;

	rm.sum_smoothed_load = 0;

	list_for_each_safe(pos, q, &rm.managed_cpus->online_list) {
		managed_cpu = list_entry(pos, struct managed_cpu, online_list);
		cpustat = &(kstat_cpu(managed_cpu->cpu).cpustat);
		if (managed_cpu->rel_total) { /* First time we pass here, no delta */
			managed_cpu->rel_user =
				cputime64_sub(cpustat->user, managed_cpu->rel_user);
			managed_cpu->rel_system =
				cputime64_sub(cpustat->system, managed_cpu->rel_system);
			managed_cpu->rel_total =
				cputime64_sub(now, managed_cpu->rel_total);

			managed_cpu->cpu_load =
				((unsigned long) (managed_cpu->rel_user + managed_cpu->rel_system)) * MAX_LOAD /
				((unsigned long) (managed_cpu->rel_total));

			rm_update_smoothed_load(managed_cpu);
			rm.sum_smoothed_load += managed_cpu->smoothed_load;
		}
		managed_cpu->rel_user = cpustat->user;
		managed_cpu->rel_system = cpustat->system;
		managed_cpu->rel_total  = now;
		++nr_cpus;
	}

	return nr_cpus;
}

/**
 * Returns the less loaded cpu.
 */
static int rm_get_laziest_cpu(void)
{
	struct managed_cpu *cpu;
	struct list_head *pos;
	int laziest_cpu = 0;
	int min_load = MAX_LOAD + 1;

	list_for_each(pos, &rm.managed_cpus->online_list) {
		cpu = list_entry(pos, struct managed_cpu, online_list);
		if (cpu->cpu && cpu->cpu_load < min_load) {
			laziest_cpu = cpu->cpu;
			min_load = cpu->cpu_load;
		}
	}

	return laziest_cpu;
}

/**
 * Kills the less loaded cpu and updates online_list and killed_list.
 */
static void rm_kill_cpu(void)
{
	struct managed_cpu *cpu;
	struct list_head *pos, *q;
	int laziest_cpu;

	laziest_cpu = rm_get_laziest_cpu();

	list_for_each_safe(pos, q, &rm.managed_cpus->online_list) {
		cpu = list_entry(pos, struct managed_cpu, online_list);
		if (cpu->cpu == laziest_cpu) {
			list_del(pos);
			cpu->cpu_load = MAX_LOAD / 2;
			cpu->smoothed_load = MAX_LOAD / 2;
			list_add(&cpu->killed_list, &rm.managed_cpus->killed_list);
			break;
		}
	}

	++rm.nr_cpus_killed;
	cpu_down(laziest_cpu);
}

/**
 * Wakes up a precedly killed cpu. The first one in the killed list is choosed,
 * and if the killed_list is empty, it returns without having done anything.
 */
static void rm_birth_cpu(void)
{
 	struct managed_cpu *cpu;
	struct list_head *pos, *q;

	list_for_each_safe(pos, q, &rm.managed_cpus->killed_list) {
		cpu = list_entry(pos, struct managed_cpu, killed_list);

		list_del(pos);

		list_add(&cpu->online_list, &rm.managed_cpus->online_list);

		--rm.nr_cpus_killed;
		cpu_up(cpu->cpu);
		return;
	}
}

/**
 * Decides if a cpu should be waken up or killed depending on the system load
 * and quality of service set by the user.
 * To kill a cpu, it tries to estimate the percentage of loss if it'd kill a
 * cpu, and if this percentage is less than the quality of service, it kills a
 * cpu.
 * To wake a cpu, it checks that the system is heavily loaded (>
 * HIGH_LOAD_LIMIT) _and_ that the average number of tasks running is greater
 * than the opposite of the quality of service.
 */
static int rm_adjust_nr_cpus(unsigned int nr_cpus)
{
	unsigned long predicted_qof =
		(MAX_LOAD * (nr_cpus - 1)) * MAX_LOAD / (rm.sum_smoothed_load + 1);

	if (nr_cpus > 1 && predicted_qof >= rm.qof) {
		rm_kill_cpu();
		return 1;
	} else if (rm.nr_cpus_killed &&
		   rm.sum_smoothed_load > HIGH_LOAD_LIMIT &&
		   rm.nr_running / nr_cpus > MAX_LOAD / (rm.qof + 1)) {
		rm_birth_cpu();
		return 1;
	}

	return 0;
}

/**
 * Loops at a periodicity fixed by the user in the rm.periodicity parameter. It
 * represents the main body of the ressource manager : it updates all
 * indicators used to guess if a cpu should be killed/waken up and call
 * rm_adjust_nr_cpus.
 */
static int cpu_killer_thread(void* data)
{
	unsigned int nr_cpus = 0;

	while (!kthread_should_stop()) {

 		nr_cpus = rm_update_cpus_load();
 		rm.nr_running = (unsigned int) nr_running();

 		rm_adjust_nr_cpus(nr_cpus);

		set_current_state(TASK_UNINTERRUPTIBLE);
		if (schedule_timeout(HZ / (1000 / rm.periodicity)) != 0)
			printk(KERN_INFO "RM: early schedule_timeout\n");
	}

	return 0;
}

static int led0_read_proc(char *page, char **start, off_t off,
			    int count, int *eof, void *data)
{
	return snprintf(page, count, "unknown\n");
}

static int led0_write_proc(struct file *file, const char __user *buffer,
			     unsigned long count, void *data)
{
	switch (buffer[0]) {
	case '0':
		leon_store_reg(PIO_BADDR + CODR_OFFSET, 1 << 6);
		break;
	case '1':
		leon_store_reg(PIO_BADDR + SODR_OFFSET, 1 << 6);
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static int led1_read_proc(char *page, char **start, off_t off,
			    int count, int *eof, void *data)
{
	return snprintf(page, count, "unknown\n");
}

static int led1_write_proc(struct file *file, const char __user *buffer,
			     unsigned long count, void *data)
{
	switch (buffer[0]) {
	case '0':
		leon_store_reg(PIO_BADDR + CODR_OFFSET, 1 << 7);
		break;
	case '1':
		leon_store_reg(PIO_BADDR + SODR_OFFSET, 1 << 7);
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static int active_read_proc(char *page, char **start, off_t off,
			    int count, int *eof, void *data)
{
	return snprintf(page, count, "%d\n", rm.active);
}

static int active_write_proc(struct file *file, const char __user *buffer,
			     unsigned long count, void *data)
{
	switch (buffer[0]) {
	case '0':
		if (rm.active) {
			printk(KERN_INFO "RM: deactivated.\n");
			rm.active = 0;
			kthread_stop(rm.killer_thread);
			while (rm.nr_cpus_killed)
				rm_birth_cpu();
		}
		break;
	case '1':
		if (!(rm.active)) {
			printk(KERN_INFO "RM: activated.\n");
			rm.active = 1;
			rm.killer_thread = kthread_run(cpu_killer_thread, NULL, "cpu_killer_thread");
		}
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static int qof_read_proc(char *page, char **start, off_t off,
			 int count, int *eof, void *data)
{
	return snprintf(page, count, "%d\n", rm.qof);
}

static int qof_write_proc(struct file *file, const char __user *buffer,
					 unsigned long count, void *data)
{
	unsigned long value = simple_strtoul(buffer, NULL, 10);

	if (value > MAX_LOAD)
		return -EINVAL;

	rm.qof = value;

	return count;
}

static int nr_cpus_killed_read_proc(char *page, char **start, off_t off,
				    int count, int *eof, void *data)
{
	return snprintf(page, count, "%d\n", rm.nr_cpus_killed);
}

/**
 * Creates the proctree needed for the user to activate/deactivate the
 * ressource manager, and to customize it.
 */
static int rm_init_proc(void)
{
	struct proc_dir_entry *tmp_entry;

	if (!(rm.proc_dir = proc_mkdir("rm", NULL)))
		goto error;

	if (!(tmp_entry = create_proc_entry("active", 0600, rm.proc_dir)))
		goto error;
	tmp_entry->data = NULL;
	tmp_entry->read_proc = active_read_proc;
	tmp_entry->write_proc = active_write_proc;

	if (!(tmp_entry = create_proc_entry("qof", 0600, rm.proc_dir)))
		goto error_qof;
	tmp_entry->data = NULL;
	tmp_entry->read_proc = qof_read_proc;
	tmp_entry->write_proc = qof_write_proc;

	if (!(tmp_entry = create_proc_entry("nr_cpus_killed", 0400, rm.proc_dir)))
		goto error_nr;
	tmp_entry->data = NULL;
	tmp_entry->read_proc = nr_cpus_killed_read_proc;
	tmp_entry->write_proc = NULL;

	if (!(tmp_entry = create_proc_entry("led0", 0600, rm.proc_dir)))
		goto error_led0;
	tmp_entry->data = NULL;
	tmp_entry->read_proc = led0_read_proc;
	tmp_entry->write_proc = led0_write_proc;

	if (!(tmp_entry = create_proc_entry("led1", 0600, rm.proc_dir)))
		goto error_led1;
	tmp_entry->data = NULL;
	tmp_entry->read_proc = led1_read_proc;
	tmp_entry->write_proc = led1_write_proc;

	return 0;

 error_led1:
	remove_proc_entry("led0", rm.proc_dir);
 error_led0:
	remove_proc_entry("nr_cpus_killed", rm.proc_dir);
 error_nr:
	remove_proc_entry("active", rm.proc_dir);
 error_qof:
	remove_proc_entry("qof", rm.proc_dir);
 error:
	if (rm.proc_dir)
		remove_proc_entry("rm", rm.proc_dir->parent);
	return -EBUSY;
}

/**
 * Removes the proctree created by the ressource manager.
 */
static void rm_exit_proc(void)
{
	remove_proc_entry("qof", rm.proc_dir);
	remove_proc_entry("active", rm.proc_dir);
	remove_proc_entry("nr_cpus_killed", rm.proc_dir);
	remove_proc_entry("led0", rm.proc_dir);
	remove_proc_entry("led1", rm.proc_dir);
	remove_proc_entry("rm", rm.proc_dir->parent);
}

/**
 * Initialize the lists of managed cpus by the ressource manager.
 */
static int rm_init_list(void)
{
	struct managed_cpu *managed_cpu;
	struct managed_cpu *tmp_cpu;
	struct list_head *pos, *q;
	int cpu = 0;

	managed_cpu = kzalloc(sizeof(*managed_cpu), GFP_KERNEL);
	if (!managed_cpu)
		goto early_error;
	INIT_LIST_HEAD(&managed_cpu->online_list);
	INIT_LIST_HEAD(&managed_cpu->killed_list);

	for_each_online_cpu(cpu) {
		tmp_cpu = kzalloc(sizeof(*tmp_cpu), GFP_KERNEL);
		if (!tmp_cpu)
			goto error;
		tmp_cpu->cpu = cpu;
		tmp_cpu->cpu_load = MAX_LOAD / 2;
		tmp_cpu->smoothed_load = MAX_LOAD / 2;
		list_add(&tmp_cpu->online_list, &managed_cpu->online_list);
	}

	rm.managed_cpus = managed_cpu;

	return 0;

 error:
	list_for_each_safe(pos, q, &managed_cpu->online_list) {
		tmp_cpu = list_entry(pos, struct managed_cpu, online_list);
		list_del(pos);
		kfree(tmp_cpu);
	}
	kfree(managed_cpu);
 early_error:
	return -ENOMEM;

}

/**
 * Destroyes precedly created lists by the module.
 */
static void rm_exit_list(void)
{
	struct managed_cpu *tmp_cpu;
	struct list_head *pos, *q;

	list_for_each_safe(pos, q, &rm.managed_cpus->online_list) {
		tmp_cpu = list_entry(pos, struct managed_cpu, online_list);
		list_del(pos);
		kfree(tmp_cpu);
	}
	kfree(rm.managed_cpus);
}

static int rm_init_pio(void)
{
	unsigned long pmsr = leon_load_reg(PIO_BADDR + PMSR_OFFSET);

	/* Start PIO clock */
	if (!(pmsr & 1))
		leon_store_reg(PIO_BADDR + ECR_OFFSET, 1);

	/* Enable led0 and led1 */
	leon_store_reg(PIO_BADDR + PER_OFFSET, 1 << 6);
	leon_store_reg(PIO_BADDR + PER_OFFSET, 1 << 7);

	/* Set led0 and led1 to ouput mode */
	leon_store_reg(PIO_BADDR + OER_OFFSET, 1 << 6);
	leon_store_reg(PIO_BADDR + OER_OFFSET, 1 << 7);

	/* Turn on led0 and led1 */
	/* leon_store_reg(PIO_BADDR + CODR_OFFSET, 1 << 6); */
	/* leon_store_reg(PIO_BADDR + CODR_OFFSET, 1 << 7); */

	return 0;
}

/**
 * This callbacks is triggered when a cpu is added/removed to/from the
 * system. It could either be by the admin or by the ressource manager
 * itself. If it's by the user, this routines is in charge of updating the
 * ressource manager lists.
 */
static int rm_hotplug_callback(struct notifier_block *nfb,
			       unsigned long action, void *data)
{
	struct managed_cpu *tmp_cpu;
	struct list_head *pos, *q;
	int cpu = (int) data;

	switch (action) {
	case CPU_DOWN_PREPARE:
		/*
		 * If the cpu has been killed by a user, it should still be
		 * present in our online_list, so we remove it.
		 */
		list_for_each_safe(pos, q, &rm.managed_cpus->online_list) {
			tmp_cpu = list_entry(pos, struct managed_cpu, online_list);
			if (tmp_cpu->cpu == cpu) {
				list_del(pos);
				kfree(tmp_cpu);
				return NOTIFY_OK;
			}
		}
	case CPU_DEAD:
	case CPU_UP_PREPARE:
		return NOTIFY_OK;
	case CPU_ONLINE:
		list_for_each_safe(pos, q, &rm.managed_cpus->online_list) {
			tmp_cpu = list_entry(pos, struct managed_cpu, online_list);
			if (tmp_cpu->cpu == cpu)
				return NOTIFY_OK;
		}

		/* The user has just hotplugged a new cpu */
		tmp_cpu = kzalloc(sizeof(*tmp_cpu), GFP_KERNEL);
		tmp_cpu->cpu = cpu;
		tmp_cpu->cpu_load = MAX_LOAD / 2;
		tmp_cpu->smoothed_load = MAX_LOAD / 2;
		list_add(&tmp_cpu->online_list, &rm.managed_cpus->online_list);

		return NOTIFY_OK;
	case CPU_UP_CANCELED:
	case CPU_DOWN_FAILED:
		/* TODO treat those cases */
		printk(KERN_INFO "RM: cpu up/down failed :"
		       " not supported yet this is gonna be a mess...\n");
	default:
		return NOTIFY_DONE;
	}

}

static struct notifier_block rm_hotplug_notifier = {
	.notifier_call = rm_hotplug_callback,
	.priority = 1
};

int __init rm_init(void)
{
	int err;

	if ((err = rm_init_proc()))
		goto error;
	if ((err = rm_init_list()))
		goto error;
	if ((err = rm_init_pio()))
		goto error;

	register_cpu_notifier(&rm_hotplug_notifier);

	printk(KERN_INFO "RM: started, HZ=%d\n", HZ);

	return 0;

 error:
	return err;
}

void __exit rm_exit(void)
{
	if (rm.active) {
		rm.active = 0;
		kthread_stop(rm.killer_thread);
		while (rm.nr_cpus_killed)
			rm_birth_cpu();
	}

	rm_exit_proc();
	rm_exit_list();

	unregister_cpu_notifier(&rm_hotplug_notifier);

	printk(KERN_INFO "RM: stopped\n");
}

module_init(rm_init);
module_exit(rm_exit);

#endif /* !RM_C_ */
