/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright IBM Corp. 2007
 *
 * Authors: Hollis Blanchard <hollisb@us.ibm.com>
 *          Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <asm/cputable.h>
#include <asm/uaccess.h>
#include <asm/kvm_ppc.h>
#include <asm/tlbflush.h>
#include "../mm/mmu_decl.h"


gfn_t unalias_gfn(struct kvm *kvm, gfn_t gfn)
{
	return gfn;
}

int kvm_cpu_has_interrupt(struct kvm_vcpu *v)
{
	return !!(v->arch.pending_exceptions);
}

int kvm_arch_vcpu_runnable(struct kvm_vcpu *v)
{
	return !(v->arch.msr & MSR_WE);
}


int kvmppc_emulate_mmio(struct kvm_run *run, struct kvm_vcpu *vcpu)
{
	enum emulation_result er;
	int r;

	er = kvmppc_emulate_instruction(run, vcpu);
	switch (er) {
	case EMULATE_DONE:
		/* Future optimization: only reload non-volatiles if they were
		 * actually modified. */
		r = RESUME_GUEST_NV;
		break;
	case EMULATE_DO_MMIO:
		run->exit_reason = KVM_EXIT_MMIO;
		/* We must reload nonvolatiles because "update" load/store
		 * instructions modify register state. */
		/* Future optimization: only reload non-volatiles if they were
		 * actually modified. */
		r = RESUME_HOST_NV;
		break;
	case EMULATE_FAIL:
		/* XXX Deliver Program interrupt to guest. */
		printk(KERN_EMERG "%s: emulation failed (%08x)\n", __func__,
		       vcpu->arch.last_inst);
		r = RESUME_HOST;
		break;
	default:
		BUG();
	}

	return r;
}

void kvm_arch_hardware_enable(void *garbage)
{
}

void kvm_arch_hardware_disable(void *garbage)
{
}

int kvm_arch_hardware_setup(void)
{
	return 0;
}

void kvm_arch_hardware_unsetup(void)
{
}

void kvm_arch_check_processor_compat(void *rtn)
{
	int r;

	if (strcmp(cur_cpu_spec->platform, "ppc440") == 0)
		r = 0;
	else
		r = -ENOTSUPP;

	*(int *)rtn = r;
}

struct kvm *kvm_arch_create_vm(void)
{
	struct kvm *kvm;

	kvm = kzalloc(sizeof(struct kvm), GFP_KERNEL);
	if (!kvm)
		return ERR_PTR(-ENOMEM);

	return kvm;
}

static void kvmppc_free_vcpus(struct kvm *kvm)
{
	unsigned int i;

	for (i = 0; i < KVM_MAX_VCPUS; ++i) {
		if (kvm->vcpus[i]) {
			kvm_arch_vcpu_free(kvm->vcpus[i]);
			kvm->vcpus[i] = NULL;
		}
	}
}

void kvm_arch_destroy_vm(struct kvm *kvm)
{
	kvmppc_free_vcpus(kvm);
	kvm_free_physmem(kvm);
	kfree(kvm);
}

int kvm_dev_ioctl_check_extension(long ext)
{
	int r;

	switch (ext) {
	case KVM_CAP_USER_MEMORY:
		r = 1;
		break;
	case KVM_CAP_COALESCED_MMIO:
		r = KVM_COALESCED_MMIO_PAGE_OFFSET;
		break;
	default:
		r = 0;
		break;
	}
	return r;

}

long kvm_arch_dev_ioctl(struct file *filp,
                        unsigned int ioctl, unsigned long arg)
{
	return -EINVAL;
}

int kvm_arch_set_memory_region(struct kvm *kvm,
                               struct kvm_userspace_memory_region *mem,
                               struct kvm_memory_slot old,
                               int user_alloc)
{
	return 0;
}

void kvm_arch_flush_shadow(struct kvm *kvm)
{
}

struct kvm_vcpu *kvm_arch_vcpu_create(struct kvm *kvm, unsigned int id)
{
	struct kvm_vcpu *vcpu;
	int err;

	vcpu = kmem_cache_zalloc(kvm_vcpu_cache, GFP_KERNEL);
	if (!vcpu) {
		err = -ENOMEM;
		goto out;
	}

	err = kvm_vcpu_init(vcpu, kvm, id);
	if (err)
		goto free_vcpu;

	return vcpu;

free_vcpu:
	kmem_cache_free(kvm_vcpu_cache, vcpu);
out:
	return ERR_PTR(err);
}

void kvm_arch_vcpu_free(struct kvm_vcpu *vcpu)
{
	kvm_vcpu_uninit(vcpu);
	kmem_cache_free(kvm_vcpu_cache, vcpu);
}

void kvm_arch_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	kvm_arch_vcpu_free(vcpu);
}

int kvm_cpu_has_pending_timer(struct kvm_vcpu *vcpu)
{
	unsigned int priority = exception_priority[BOOKE_INTERRUPT_DECREMENTER];

	return test_bit(priority, &vcpu->arch.pending_exceptions);
}

static void kvmppc_decrementer_func(unsigned long data)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *)data;

	kvmppc_queue_exception(vcpu, BOOKE_INTERRUPT_DECREMENTER);

	if (waitqueue_active(&vcpu->wq)) {
		wake_up_interruptible(&vcpu->wq);
		vcpu->stat.halt_wakeup++;
	}
}

int kvm_arch_vcpu_init(struct kvm_vcpu *vcpu)
{
	setup_timer(&vcpu->arch.dec_timer, kvmppc_decrementer_func,
	            (unsigned long)vcpu);

	return 0;
}

void kvm_arch_vcpu_uninit(struct kvm_vcpu *vcpu)
{
	kvmppc_core_destroy_mmu(vcpu);
}

/* Note: clearing MSR[DE] just means that the debug interrupt will not be
 * delivered *immediately*. Instead, it simply sets the appropriate DBSR bits.
 * If those DBSR bits are still set when MSR[DE] is re-enabled, the interrupt
 * will be delivered as an "imprecise debug event" (which is indicated by
 * DBSR[IDE].
 */
static void kvmppc_disable_debug_interrupts(void)
{
	mtmsr(mfmsr() & ~MSR_DE);
}

static void kvmppc_restore_host_debug_state(struct kvm_vcpu *vcpu)
{
	kvmppc_disable_debug_interrupts();

	mtspr(SPRN_IAC1, vcpu->arch.host_iac[0]);
	mtspr(SPRN_IAC2, vcpu->arch.host_iac[1]);
	mtspr(SPRN_IAC3, vcpu->arch.host_iac[2]);
	mtspr(SPRN_IAC4, vcpu->arch.host_iac[3]);
	mtspr(SPRN_DBCR1, vcpu->arch.host_dbcr1);
	mtspr(SPRN_DBCR2, vcpu->arch.host_dbcr2);
	mtspr(SPRN_DBCR0, vcpu->arch.host_dbcr0);
	mtmsr(vcpu->arch.host_msr);
}

static void kvmppc_load_guest_debug_registers(struct kvm_vcpu *vcpu)
{
	struct kvm_guest_debug *dbg = &vcpu->guest_debug;
	u32 dbcr0 = 0;

	vcpu->arch.host_msr = mfmsr();
	kvmppc_disable_debug_interrupts();

	/* Save host debug register state. */
	vcpu->arch.host_iac[0] = mfspr(SPRN_IAC1);
	vcpu->arch.host_iac[1] = mfspr(SPRN_IAC2);
	vcpu->arch.host_iac[2] = mfspr(SPRN_IAC3);
	vcpu->arch.host_iac[3] = mfspr(SPRN_IAC4);
	vcpu->arch.host_dbcr0 = mfspr(SPRN_DBCR0);
	vcpu->arch.host_dbcr1 = mfspr(SPRN_DBCR1);
	vcpu->arch.host_dbcr2 = mfspr(SPRN_DBCR2);

	/* set registers up for guest */

	if (dbg->bp[0]) {
		mtspr(SPRN_IAC1, dbg->bp[0]);
		dbcr0 |= DBCR0_IAC1 | DBCR0_IDM;
	}
	if (dbg->bp[1]) {
		mtspr(SPRN_IAC2, dbg->bp[1]);
		dbcr0 |= DBCR0_IAC2 | DBCR0_IDM;
	}
	if (dbg->bp[2]) {
		mtspr(SPRN_IAC3, dbg->bp[2]);
		dbcr0 |= DBCR0_IAC3 | DBCR0_IDM;
	}
	if (dbg->bp[3]) {
		mtspr(SPRN_IAC4, dbg->bp[3]);
		dbcr0 |= DBCR0_IAC4 | DBCR0_IDM;
	}

	mtspr(SPRN_DBCR0, dbcr0);
	mtspr(SPRN_DBCR1, 0);
	mtspr(SPRN_DBCR2, 0);
}

void kvm_arch_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	int i;

	if (vcpu->guest_debug.enabled)
		kvmppc_load_guest_debug_registers(vcpu);

	/* Mark every guest entry in the shadow TLB entry modified, so that they
	 * will all be reloaded on the next vcpu run (instead of being
	 * demand-faulted). */
	for (i = 0; i <= tlb_44x_hwater; i++)
		kvmppc_tlbe_set_modified(vcpu, i);
}

void kvm_arch_vcpu_put(struct kvm_vcpu *vcpu)
{
	if (vcpu->guest_debug.enabled)
		kvmppc_restore_host_debug_state(vcpu);

	/* Don't leave guest TLB entries resident when being de-scheduled. */
	/* XXX It would be nice to differentiate between heavyweight exit and
	 * sched_out here, since we could avoid the TLB flush for heavyweight
	 * exits. */
	_tlbil_all();
}

int kvm_arch_vcpu_ioctl_debug_guest(struct kvm_vcpu *vcpu,
                                    struct kvm_debug_guest *dbg)
{
	int i;

	vcpu->guest_debug.enabled = dbg->enabled;
	if (vcpu->guest_debug.enabled) {
		for (i=0; i < ARRAY_SIZE(vcpu->guest_debug.bp); i++) {
			if (dbg->breakpoints[i].enabled)
				vcpu->guest_debug.bp[i] = dbg->breakpoints[i].address;
			else
				vcpu->guest_debug.bp[i] = 0;
		}
	}

	return 0;
}

static void kvmppc_complete_dcr_load(struct kvm_vcpu *vcpu,
                                     struct kvm_run *run)
{
	u32 *gpr = &vcpu->arch.gpr[vcpu->arch.io_gpr];
	*gpr = run->dcr.data;
}

static void kvmppc_complete_mmio_load(struct kvm_vcpu *vcpu,
                                      struct kvm_run *run)
{
	u32 *gpr = &vcpu->arch.gpr[vcpu->arch.io_gpr];

	if (run->mmio.len > sizeof(*gpr)) {
		printk(KERN_ERR "bad MMIO length: %d\n", run->mmio.len);
		return;
	}

	if (vcpu->arch.mmio_is_bigendian) {
		switch (run->mmio.len) {
		case 4: *gpr = *(u32 *)run->mmio.data; break;
		case 2: *gpr = *(u16 *)run->mmio.data; break;
		case 1: *gpr = *(u8 *)run->mmio.data; break;
		}
	} else {
		/* Convert BE data from userland back to LE. */
		switch (run->mmio.len) {
		case 4: *gpr = ld_le32((u32 *)run->mmio.data); break;
		case 2: *gpr = ld_le16((u16 *)run->mmio.data); break;
		case 1: *gpr = *(u8 *)run->mmio.data; break;
		}
	}
}

int kvmppc_handle_load(struct kvm_run *run, struct kvm_vcpu *vcpu,
                       unsigned int rt, unsigned int bytes, int is_bigendian)
{
	if (bytes > sizeof(run->mmio.data)) {
		printk(KERN_ERR "%s: bad MMIO length: %d\n", __func__,
		       run->mmio.len);
	}

	run->mmio.phys_addr = vcpu->arch.paddr_accessed;
	run->mmio.len = bytes;
	run->mmio.is_write = 0;

	vcpu->arch.io_gpr = rt;
	vcpu->arch.mmio_is_bigendian = is_bigendian;
	vcpu->mmio_needed = 1;
	vcpu->mmio_is_write = 0;

	return EMULATE_DO_MMIO;
}

int kvmppc_handle_store(struct kvm_run *run, struct kvm_vcpu *vcpu,
                        u32 val, unsigned int bytes, int is_bigendian)
{
	void *data = run->mmio.data;

	if (bytes > sizeof(run->mmio.data)) {
		printk(KERN_ERR "%s: bad MMIO length: %d\n", __func__,
		       run->mmio.len);
	}

	run->mmio.phys_addr = vcpu->arch.paddr_accessed;
	run->mmio.len = bytes;
	run->mmio.is_write = 1;
	vcpu->mmio_needed = 1;
	vcpu->mmio_is_write = 1;

	/* Store the value at the lowest bytes in 'data'. */
	if (is_bigendian) {
		switch (bytes) {
		case 4: *(u32 *)data = val; break;
		case 2: *(u16 *)data = val; break;
		case 1: *(u8  *)data = val; break;
		}
	} else {
		/* Store LE value into 'data'. */
		switch (bytes) {
		case 4: st_le32(data, val); break;
		case 2: st_le16(data, val); break;
		case 1: *(u8 *)data = val; break;
		}
	}

	return EMULATE_DO_MMIO;
}

int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	int r;
	sigset_t sigsaved;

	vcpu_load(vcpu);

	if (vcpu->sigset_active)
		sigprocmask(SIG_SETMASK, &vcpu->sigset, &sigsaved);

	if (vcpu->mmio_needed) {
		if (!vcpu->mmio_is_write)
			kvmppc_complete_mmio_load(vcpu, run);
		vcpu->mmio_needed = 0;
	} else if (vcpu->arch.dcr_needed) {
		if (!vcpu->arch.dcr_is_write)
			kvmppc_complete_dcr_load(vcpu, run);
		vcpu->arch.dcr_needed = 0;
	}

	kvmppc_check_and_deliver_interrupts(vcpu);

	local_irq_disable();
	kvm_guest_enter();
	r = __kvmppc_vcpu_run(run, vcpu);
	kvm_guest_exit();
	local_irq_enable();

	if (vcpu->sigset_active)
		sigprocmask(SIG_SETMASK, &sigsaved, NULL);

	vcpu_put(vcpu);

	return r;
}

int kvm_vcpu_ioctl_interrupt(struct kvm_vcpu *vcpu, struct kvm_interrupt *irq)
{
	kvmppc_queue_exception(vcpu, BOOKE_INTERRUPT_EXTERNAL);

	if (waitqueue_active(&vcpu->wq)) {
		wake_up_interruptible(&vcpu->wq);
		vcpu->stat.halt_wakeup++;
	}

	return 0;
}

int kvm_arch_vcpu_ioctl_get_mpstate(struct kvm_vcpu *vcpu,
                                    struct kvm_mp_state *mp_state)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_set_mpstate(struct kvm_vcpu *vcpu,
                                    struct kvm_mp_state *mp_state)
{
	return -EINVAL;
}

long kvm_arch_vcpu_ioctl(struct file *filp,
                         unsigned int ioctl, unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;
	long r;

	switch (ioctl) {
	case KVM_INTERRUPT: {
		struct kvm_interrupt irq;
		r = -EFAULT;
		if (copy_from_user(&irq, argp, sizeof(irq)))
			goto out;
		r = kvm_vcpu_ioctl_interrupt(vcpu, &irq);
		break;
	}
	default:
		r = -EINVAL;
	}

out:
	return r;
}

int kvm_vm_ioctl_get_dirty_log(struct kvm *kvm, struct kvm_dirty_log *log)
{
	return -ENOTSUPP;
}

long kvm_arch_vm_ioctl(struct file *filp,
                       unsigned int ioctl, unsigned long arg)
{
	long r;

	switch (ioctl) {
	default:
		r = -EINVAL;
	}

	return r;
}

int kvm_arch_init(void *opaque)
{
	return 0;
}

void kvm_arch_exit(void)
{
}
