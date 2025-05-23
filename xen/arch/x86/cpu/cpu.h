/* attempt to consolidate cpu attributes */

#ifndef X86_CPU_H
#define X86_CPU_H

struct cpu_dev {
	void		(*c_early_init)(struct cpuinfo_x86 *c);
	void		(*c_init)(struct cpuinfo_x86 * c);
};

extern const struct cpu_dev intel_cpu_dev, amd_cpu_dev, centaur_cpu_dev,
    shanghai_cpu_dev, hygon_cpu_dev;

extern bool opt_arat;
extern unsigned int opt_cpuid_mask_ecx, opt_cpuid_mask_edx;
extern unsigned int opt_cpuid_mask_xsave_eax;
extern unsigned int opt_cpuid_mask_ext_ecx, opt_cpuid_mask_ext_edx;

extern int get_model_name(struct cpuinfo_x86 *c);
extern void display_cacheinfo(struct cpuinfo_x86 *c);

extern void detect_ht(struct cpuinfo_x86 *c);
extern bool detect_extended_topology(struct cpuinfo_x86 *c);

void cf_check early_init_amd(struct cpuinfo_x86 *c);
void amd_log_freq(const struct cpuinfo_x86 *c);
void amd_init_lfence(struct cpuinfo_x86 *c);
void amd_init_ssbd(const struct cpuinfo_x86 *c);
void amd_init_spectral_chicken(void);
void detect_zen2_null_seg_behaviour(void);

void intel_unlock_cpuid_leaves(struct cpuinfo_x86 *c);

#endif /* X86_CPU_H */
