module;

#ifndef CONFLUX_WORK_CARRIER_MODEL_A
	#define CONFLUX_WORK_CARRIER_MODEL_A 0
#endif

#ifndef CONFLUX_WORK_CARRIER_MODEL_B
	#define CONFLUX_WORK_CARRIER_MODEL_B 0
#endif

export module conflux.work.carrier.flags;
export namespace conflux::work::carrier {

inline constexpr bool kModelAEnabled = CONFLUX_WORK_CARRIER_MODEL_A != 0;
inline constexpr bool kModelBEnabled = CONFLUX_WORK_CARRIER_MODEL_B != 0;

} // namespace conflux::work::carrier
