
/**
 * @file mc_att_control_indi_main.cpp
 * Adaptive Incremental Nonlinear Dynamic Inversion for Attitude Control of Micro Aerial Vehicles
 *
 * @author leishanshan		<m15128259854@163.com>
 **/

#include <conversion/rotation.h>
#include <drivers/drv_hrt.h>
#include <lib/geo/geo.h>
#include <lib/mathlib/mathlib.h>
#include <lib/tailsitter_recovery/tailsitter_recovery.h>
#include <px4_config.h>
#include <px4_defines.h>
#include <px4_posix.h>
#include <px4_tasks.h>
#include <systemlib/circuit_breaker.h>
#include <systemlib/err.h>
#include <systemlib/param/param.h>
#include <systemlib/perf_counter.h>
#include <systemlib/systemlib.h>
#include <uORB/topics/actuator_armed.h>
#include <uORB/topics/actuator_controls.h>
#include <uORB/topics/battery_status.h>
#include <uORB/topics/control_state.h>
#include <uORB/topics/manual_control_setpoint.h>
#include <uORB/topics/mc_att_ctrl_status.h>
#include <uORB/topics/multirotor_motor_limits.h>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/sensor_correction.h>
#include <uORB/topics/sensor_gyro.h>
#include <uORB/topics/vehicle_attitude_setpoint.h>
#include <uORB/topics/vehicle_control_mode.h>
#include <uORB/topics/vehicle_rates_setpoint.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/indi.h>
#include <uORB/uORB.h>
#include "airframe.h"
#include "math/pprz_algebra_float.h"
#include "math/pprz_algebra_int.h"
#include "filters/low_pass_filter.h"

/**
 * Multicopter attitude control app start / stop handling function
 *
 * @ingroup apps
 */
extern "C" __EXPORT int mc_att_control_indi_main(int argc, char *argv[]);


//=============================INDI==========================

#if !defined(STABILIZATION_INDI_ACT_DYN_P) && !defined(STABILIZATION_INDI_ACT_DYN_Q) && !defined(STABILIZATION_INDI_ACT_DYN_R)
#error You have to define the first order time constant of the actuator dynamics!
#endif

// these parameters are used in the filtering of the angular acceleration
// define them in the airframe file if different values are required
#ifndef STABILIZATION_INDI_FILT_CUTOFF
#define STABILIZATION_INDI_FILT_CUTOFF 8.0
#endif

// the yaw sometimes requires more filtering
#ifndef STABILIZATION_INDI_FILT_CUTOFF_R
#define STABILIZATION_INDI_FILT_CUTOFF_R STABILIZATION_INDI_FILT_CUTOFF
#endif

#ifndef STABILIZATION_INDI_MAX_RATE
#define STABILIZATION_INDI_MAX_RATE 6.0
#endif

#ifndef STABILIZATION_INDI_MAX_R
#define STABILIZATION_INDI_MAX_R STABILIZATION_ATTITUDE_SP_MAX_R
#endif

#ifndef STABILIZATION_INDI_ESTIMATION_FILT_CUTOFF
#define STABILIZATION_INDI_ESTIMATION_FILT_CUTOFF 4.0
#endif
//The G values are scaled to avoid numerical problems during the estimation
#define INDI_EST_SCALE 0.0001
#define PERIODIC_FREQUENCY 250.0
//=============================INDI==========================

//=============================PID==========================
#define YAW_DEADZONE	0.05f
#define MIN_TAKEOFF_THRUST    0.2f
#define TPA_RATE_LOWER_LIMIT 0.05f
#define MANUAL_THROTTLE_MAX_MULTICOPTER	0.9f
#define ATTITUDE_TC_DEFAULT 0.2f

#define AXIS_INDEX_ROLL 0
#define AXIS_INDEX_PITCH 1
#define AXIS_INDEX_YAW 2
#define AXIS_COUNT 3

#define MAX_GYRO_COUNT 3
//=============================PID==========================

class MulticopterAttitudeControl
{
public:
	/**
	 * Constructor
	 */
	MulticopterAttitudeControl();

	/**
	 * Destructor, also kills the main task
	 */
	~MulticopterAttitudeControl();

	/**
	 * Start the multicopter attitude control task.
	 *
	 * @return		OK on success.
	 */
	int		start();

private:
    //=============================INDI==========================

    struct ReferenceSystem {
      float err_p;
      float err_q;
      float err_r;
      float rate_p;
      float rate_q;
      float rate_r;
    };

    struct IndiEstimation {
      Butterworth2LowPass u[3];
      Butterworth2LowPass rate[3];
      float rate_d[3];
      float rate_dd[3];
      float u_d[3];
      float u_dd[3];
      struct FloatRates g1;
      float g2;
      float mu;
    };

    struct IndiVariables {
      struct FloatRates angular_accel_ref;
      struct FloatRates du;
      struct FloatRates u_in;
      struct FloatRates u_act_dyn;
      float rate_d[3];

      Butterworth2LowPass u[3];
      Butterworth2LowPass rate[3];
      struct FloatRates g1;
      float g2;

      struct ReferenceSystem reference_acceleration;

      bool adaptive;             ///< Enable adataptive estimation
      float max_rate;            ///< Maximum rate in rate control in rad/s
      float attitude_max_yaw_rate; ///< Maximum yaw rate in atttiude control in rad/s
      struct IndiEstimation est; ///< Estimation parameters for adaptive INDI
    };
    struct IndiVariables indi;
    int32_t stabilization_att_indi_cmd[COMMANDS_NB];
//=============================INDI==========================
	bool	_task_should_exit;		/**< if true, task_main() should exit */
	int		_control_task;			/**< task handle */

	int		_ctrl_state_sub;		/**< control state subscription */
	int		_v_att_sp_sub;			/**< vehicle attitude setpoint subscription */
	int		_v_rates_sp_sub;		/**< vehicle rates setpoint subscription */
	int		_v_control_mode_sub;	/**< vehicle control mode subscription */
	int		_params_sub;			/**< parameter updates subscription */
	int		_manual_control_sp_sub;	/**< manual control setpoint subscription */
	int		_armed_sub;				/**< arming status subscription */
	int		_vehicle_status_sub;	/**< vehicle status subscription */
	int 	_motor_limits_sub;		/**< motor limits subscription */
	int 	_battery_status_sub;	/**< battery status subscription */
	int	_sensor_gyro_sub[MAX_GYRO_COUNT];	/**< gyro data subscription */
	int	_sensor_correction_sub;	/**< sensor thermal correction subscription */

	unsigned _gyro_count;
	int _selected_gyro;

	orb_advert_t	_v_rates_sp_pub;		/**< rate setpoint publication */
	orb_advert_t	_actuators_0_pub;		/**< attitude actuator controls publication */
	orb_advert_t	_controller_status_pub;	/**< controller status publication */
    orb_advert_t	_indi_adaptive_pub;	/**< indi adaptive status publication */

	orb_id_t _rates_sp_id;	/**< pointer to correct rates setpoint uORB metadata structure */
	orb_id_t _actuators_id;	/**< pointer to correct actuator controls0 uORB metadata structure */

	bool		_actuators_0_circuit_breaker_enabled;	/**< circuit breaker to suppress output */

	struct control_state_s				_ctrl_state;		/**< control state */
	struct vehicle_attitude_setpoint_s	_v_att_sp;			/**< vehicle attitude setpoint */
	struct vehicle_rates_setpoint_s		_v_rates_sp;		/**< vehicle rates setpoint */
	struct manual_control_setpoint_s	_manual_control_sp;	/**< manual control setpoint */
	struct vehicle_control_mode_s		_v_control_mode;	/**< vehicle control mode */
	struct actuator_controls_s			_actuators;			/**< actuator controls */
	struct actuator_armed_s				_armed;				/**< actuator arming status */
	struct vehicle_status_s				_vehicle_status;	/**< vehicle status */
	struct multirotor_motor_limits_s	_motor_limits;		/**< motor limits */
	struct mc_att_ctrl_status_s 		_controller_status; /**< controller status */
    struct indi_s                       _indi_status; /**< indi status */
    struct battery_status_s				_battery_status;	/**< battery status */
	struct sensor_gyro_s			_sensor_gyro;		/**< gyro data before thermal correctons and ekf bias estimates are applied */
	struct sensor_correction_s		_sensor_correction;		/**< sensor thermal corrections */

	union {
		struct {
			uint16_t motor_pos	: 1; // 0 - true when any motor has saturated in the positive direction
			uint16_t motor_neg	: 1; // 1 - true when any motor has saturated in the negative direction
			uint16_t roll_pos	: 1; // 2 - true when a positive roll demand change will increase saturation
			uint16_t roll_neg	: 1; // 3 - true when a negative roll demand change will increase saturation
			uint16_t pitch_pos	: 1; // 4 - true when a positive pitch demand change will increase saturation
			uint16_t pitch_neg	: 1; // 5 - true when a negative pitch demand change will increase saturation
			uint16_t yaw_pos	: 1; // 6 - true when a positive yaw demand change will increase saturation
			uint16_t yaw_neg	: 1; // 7 - true when a negative yaw demand change will increase saturation
			uint16_t thrust_pos	: 1; // 8 - true when a positive thrust demand change will increase saturation
			uint16_t thrust_neg	: 1; // 9 - true when a negative thrust demand change will increase saturation
		} flags;
		uint16_t value;
	} _saturation_status;

	perf_counter_t	_loop_perf;			/**< loop performance counter */
	perf_counter_t	_controller_latency_perf;

	math::Vector<3>		_rates_prev;	/**< angular rates on previous step */
	math::Vector<3>		_rates_sp_prev; /**< previous rates setpoint */
	math::Vector<3>		_rates_sp;		/**< angular rates setpoint */
	math::Vector<3>		_rates_int;		/**< angular rates integral error */
	float				_thrust_sp;		/**< thrust setpoint */
	math::Vector<3>		_att_control;	/**< attitude control vector */

	math::Matrix<3, 3>  _I;				/**< identity matrix */

	math::Matrix<3, 3>	_board_rotation = {};	/**< rotation matrix for the orientation that the board is mounted */

	struct {
		param_t roll_p;
		param_t roll_rate_p;
		param_t roll_rate_i;
		param_t roll_rate_integ_lim;
		param_t roll_rate_d;
		param_t roll_rate_ff;
		param_t pitch_p;
		param_t pitch_rate_p;
		param_t pitch_rate_i;
		param_t pitch_rate_integ_lim;
		param_t pitch_rate_d;
		param_t pitch_rate_ff;
		param_t tpa_breakpoint_p;
		param_t tpa_breakpoint_i;
		param_t tpa_breakpoint_d;
		param_t tpa_rate_p;
		param_t tpa_rate_i;
		param_t tpa_rate_d;
		param_t yaw_p;
		param_t yaw_rate_p;
		param_t yaw_rate_i;
		param_t yaw_rate_integ_lim;
		param_t yaw_rate_d;
		param_t yaw_rate_ff;
		param_t yaw_ff;
		param_t roll_rate_max;
		param_t pitch_rate_max;
		param_t yaw_rate_max;
		param_t yaw_auto_max;

		param_t acro_roll_max;
		param_t acro_pitch_max;
		param_t acro_yaw_max;
		param_t rattitude_thres;

		param_t vtol_type;
		param_t roll_tc;
		param_t pitch_tc;
		param_t vtol_opt_recovery_enabled;
		param_t vtol_wv_yaw_rate_scale;

		param_t bat_scale_en;

		param_t board_rotation;

		param_t board_offset[3];

        param_t use_indi;
        param_t use_indi_adaptive;
        param_t indi_adaptive_mu;
        param_t g1_p;
        param_t g1_q;
        param_t g1_r;
        param_t g2_r;
        param_t ref_err_p;
        param_t ref_err_q;
        param_t ref_err_r;
        param_t ref_rate_p;
        param_t ref_rate_q;
        param_t ref_rate_r;
        param_t act_dyn_p;
        param_t act_dyn_q;
        param_t act_dyn_r;

	}		_params_handles;		/**< handles for interesting parameters */

	struct {
		math::Vector<3> att_p;					/**< P gain for angular error */
		math::Vector<3> rate_p;				/**< P gain for angular rate error */
		math::Vector<3> rate_i;				/**< I gain for angular rate error */
		math::Vector<3> rate_int_lim;			/**< integrator state limit for rate loop */
		math::Vector<3> rate_d;				/**< D gain for angular rate error */
		math::Vector<3>	rate_ff;			/**< Feedforward gain for desired rates */
		float yaw_ff;						/**< yaw control feed-forward */

		float tpa_breakpoint_p;				/**< Throttle PID Attenuation breakpoint */
		float tpa_breakpoint_i;				/**< Throttle PID Attenuation breakpoint */
		float tpa_breakpoint_d;				/**< Throttle PID Attenuation breakpoint */
		float tpa_rate_p;					/**< Throttle PID Attenuation slope */
		float tpa_rate_i;					/**< Throttle PID Attenuation slope */
		float tpa_rate_d;					/**< Throttle PID Attenuation slope */

		float roll_rate_max;
		float pitch_rate_max;
		float yaw_rate_max;
		float yaw_auto_max;
		math::Vector<3> mc_rate_max;		/**< attitude rate limits in stabilized modes */
		math::Vector<3> auto_rate_max;		/**< attitude rate limits in auto modes */
		math::Vector<3> acro_rate_max;		/**< max attitude rates in acro mode */
		float rattitude_thres;
		int vtol_type;						/**< 0 = Tailsitter, 1 = Tiltrotor, 2 = Standard airframe */
		bool vtol_opt_recovery_enabled;
		float vtol_wv_yaw_rate_scale;			/**< Scale value [0, 1] for yaw rate setpoint  */

		int bat_scale_en;

		int board_rotation;

		float board_offset[3];

        int use_indi;
        int use_indi_adaptive;
        float indi_adaptive_mu;
        float g1_p;
        float g1_q;
        float g1_r;
        float g2_r;
        float ref_err_p;
        float ref_err_q;
        float ref_err_r;
        float ref_rate_p;
        float ref_rate_q;
        float ref_rate_r;
        float act_dyn_p;
        float act_dyn_q;
        float act_dyn_r;

	}		_params;

	TailsitterRecovery *_ts_opt_recovery;	/**< Computes optimal rates for tailsitter recovery */

	/**
	 * Update our local parameter cache.
	 */
	int			parameters_update();

	/**
	 * Check for parameter update and handle it.
	 */
	void		parameter_update_poll();

	/**
	 * Check for changes in vehicle control mode.
	 */
	void		vehicle_control_mode_poll();

	/**
	 * Check for changes in manual inputs.
	 */
	void		vehicle_manual_poll();

	/**
	 * Check for attitude setpoint updates.
	 */
	void		vehicle_attitude_setpoint_poll();

	/**
	 * Check for rates setpoint updates.
	 */
	void		vehicle_rates_setpoint_poll();

	/**
	 * Check for arming status updates.
	 */
	void		arming_status_poll();

	/**
	 * Attitude controller.
	 */
	void		control_attitude(float dt);

	/**
	 * Attitude rates controller.
	 */
	void		control_attitude_rates(float dt);

	/**
	 * Throttle PID attenuation.
	 */
	math::Vector<3> pid_attenuations(float tpa_breakpoint, float tpa_rate);
    void get_body_rates(math::Vector<3> *rates);
    void get_attitude_errors(math::Vector<3> *att_e);
//    void stabilization_indi_calc_cmd(int32_t indi_commands[], struct Int32Quat *att_err, bool rate_control);
    void auto_save_indi_param();
    void stabilization_indi_calc_cmd(int32_t indi_commands[], math::Vector<3> att_err, bool rate_control,float dt);
    void lms_estimation(float dt);
    void indi_init_filters(void);
    void stabilization_indi_init(void);
    void stabilization_indi_enter(void);
    void stabilization_indi_run(bool enable_integrator, bool rate_control,float dt);

    void filter_pqr(Butterworth2LowPass *filter, struct FloatRates *new_values);
    void finite_difference_from_filter(float *output, Butterworth2LowPass *filter,float dt);
    void finite_difference(float output[3], float new1[3], float old[3],float dt);

    //    void stabilization_indi_set_failsafe_setpoint(void);
    //    void stabilization_indi_set_rpy_setpoint_i(struct Int32Eulers *rpy);
    //    void stabilization_indi_set_earth_cmd_i(struct Int32Vect2 *cmd, int32_t heading);

	/**
	 * Check for vehicle status updates.
	 */
	void		vehicle_status_poll();

	/**
	 * Check for vehicle motor limits status.
	 */
	void		vehicle_motor_limits_poll();

	/**
	 * Check for battery status updates.
	 */
	void		battery_status_poll();

	/**
	 * Check for control state updates.
	 */
	void		control_state_poll();

	/**
	 * Check for sensor thermal correction updates.
	 */
	void		sensor_correction_poll();

	/**
	 * Shim for calling task_main from task_create.
	 */
	static void	task_main_trampoline(int argc, char *argv[]);

	/**
	 * Main attitude control task.
	 */
	void		task_main();
};

namespace mc_att_control
{

MulticopterAttitudeControl	*g_control;
}

MulticopterAttitudeControl::MulticopterAttitudeControl() :
    //INDI
    indi{},
    _task_should_exit(false),
	_control_task(-1),

	/* subscriptions */
	_ctrl_state_sub(-1),
	_v_att_sp_sub(-1),
	_v_control_mode_sub(-1),
	_params_sub(-1),
	_manual_control_sp_sub(-1),
	_armed_sub(-1),
	_vehicle_status_sub(-1),
	_motor_limits_sub(-1),
	_battery_status_sub(-1),
	_sensor_correction_sub(-1),

	/* gyro selection */
	_gyro_count(1),
	_selected_gyro(0),

	/* publications */
	_v_rates_sp_pub(nullptr),
	_actuators_0_pub(nullptr),
	_controller_status_pub(nullptr),
    _indi_adaptive_pub(nullptr),
	_rates_sp_id(nullptr),
	_actuators_id(nullptr),

	_actuators_0_circuit_breaker_enabled(false),

	_ctrl_state{},
	_v_att_sp{},
	_v_rates_sp{},
	_manual_control_sp{},
	_v_control_mode{},
	_actuators{},
	_armed{},
	_vehicle_status{},
	_motor_limits{},
	_controller_status{},
    _indi_status{},
	_battery_status{},
	_sensor_gyro{},
	_sensor_correction{},

	_saturation_status{},
	/* performance counters */
	_loop_perf(perf_alloc(PC_ELAPSED, "mc_att_control")),
	_controller_latency_perf(perf_alloc_once(PC_ELAPSED, "ctrl_latency")),
    _ts_opt_recovery(nullptr)
{

	for (uint8_t i = 0; i < MAX_GYRO_COUNT; i++) {
		_sensor_gyro_sub[i] = -1;
	}

	_vehicle_status.is_rotary_wing = true;

	_params.att_p.zero();
	_params.rate_p.zero();
	_params.rate_i.zero();
	_params.rate_int_lim.zero();
	_params.rate_d.zero();
	_params.rate_ff.zero();
	_params.yaw_ff = 0.0f;
	_params.roll_rate_max = 0.0f;
	_params.pitch_rate_max = 0.0f;
	_params.yaw_rate_max = 0.0f;
	_params.mc_rate_max.zero();
	_params.auto_rate_max.zero();
	_params.acro_rate_max.zero();
	_params.rattitude_thres = 1.0f;
	_params.vtol_opt_recovery_enabled = false;
	_params.vtol_wv_yaw_rate_scale = 1.0f;
	_params.bat_scale_en = 0;

	_params.board_rotation = 0;

	_params.board_offset[0] = 0.0f;
	_params.board_offset[1] = 0.0f;
	_params.board_offset[2] = 0.0f;
    _params.use_indi = 0;
    _params.use_indi_adaptive = 0;
    _params.indi_adaptive_mu = 0.00001;
    _params.g1_p = 0.0f;
    _params.g1_q = 0.0f;
    _params.g1_r = 0.0f;
    _params.g2_r = 0.0f;
    _params.ref_err_p = 0.0f;
    _params.ref_err_q = 0.0f;
    _params.ref_err_r = 0.0f;
    _params.ref_rate_p = 0.0f;
    _params.ref_rate_q = 0.0f;
    _params.ref_rate_r = 0.0f;
    _params.act_dyn_p = 0.0f;
    _params.act_dyn_q = 0.0f;
    _params.act_dyn_r = 0.0f;
	_rates_prev.zero();
	_rates_sp.zero();
	_rates_sp_prev.zero();
	_rates_int.zero();
	_thrust_sp = 0.0f;
	_att_control.zero();

	_I.identity();
	_board_rotation.identity();

	_params_handles.roll_p			= 	param_find("MC_ROLL_P");
	_params_handles.roll_rate_p		= 	param_find("MC_ROLLRATE_P");
	_params_handles.roll_rate_i		= 	param_find("MC_ROLLRATE_I");
	_params_handles.roll_rate_integ_lim	= 	param_find("MC_RR_INT_LIM");
	_params_handles.roll_rate_d		= 	param_find("MC_ROLLRATE_D");
	_params_handles.roll_rate_ff	= 	param_find("MC_ROLLRATE_FF");
	_params_handles.pitch_p			= 	param_find("MC_PITCH_P");
	_params_handles.pitch_rate_p	= 	param_find("MC_PITCHRATE_P");
	_params_handles.pitch_rate_i	= 	param_find("MC_PITCHRATE_I");
	_params_handles.pitch_rate_integ_lim	= 	param_find("MC_PR_INT_LIM");
	_params_handles.pitch_rate_d	= 	param_find("MC_PITCHRATE_D");
	_params_handles.pitch_rate_ff 	= 	param_find("MC_PITCHRATE_FF");
	_params_handles.tpa_breakpoint_p 	= 	param_find("MC_TPA_BREAK_P");
	_params_handles.tpa_breakpoint_i 	= 	param_find("MC_TPA_BREAK_I");
	_params_handles.tpa_breakpoint_d 	= 	param_find("MC_TPA_BREAK_D");
	_params_handles.tpa_rate_p	 	= 	param_find("MC_TPA_RATE_P");
	_params_handles.tpa_rate_i	 	= 	param_find("MC_TPA_RATE_I");
	_params_handles.tpa_rate_d	 	= 	param_find("MC_TPA_RATE_D");
	_params_handles.yaw_p			=	param_find("MC_YAW_P");
	_params_handles.yaw_rate_p		= 	param_find("MC_YAWRATE_P");
	_params_handles.yaw_rate_i		= 	param_find("MC_YAWRATE_I");
	_params_handles.yaw_rate_integ_lim	= 	param_find("MC_YR_INT_LIM");
	_params_handles.yaw_rate_d		= 	param_find("MC_YAWRATE_D");
	_params_handles.yaw_rate_ff	 	= 	param_find("MC_YAWRATE_FF");
	_params_handles.yaw_ff			= 	param_find("MC_YAW_FF");
	_params_handles.roll_rate_max	= 	param_find("MC_ROLLRATE_MAX");
	_params_handles.pitch_rate_max	= 	param_find("MC_PITCHRATE_MAX");
	_params_handles.yaw_rate_max	= 	param_find("MC_YAWRATE_MAX");
	_params_handles.yaw_auto_max	= 	param_find("MC_YAWRAUTO_MAX");
	_params_handles.acro_roll_max	= 	param_find("MC_ACRO_R_MAX");
	_params_handles.acro_pitch_max	= 	param_find("MC_ACRO_P_MAX");
	_params_handles.acro_yaw_max	= 	param_find("MC_ACRO_Y_MAX");
	_params_handles.rattitude_thres = 	param_find("MC_RATT_TH");
	_params_handles.vtol_type 		= 	param_find("VT_TYPE");
	_params_handles.roll_tc			= 	param_find("MC_ROLL_TC");
	_params_handles.pitch_tc		= 	param_find("MC_PITCH_TC");
	_params_handles.vtol_opt_recovery_enabled	= param_find("VT_OPT_RECOV_EN");
	_params_handles.vtol_wv_yaw_rate_scale		= param_find("VT_WV_YAWR_SCL");
	_params_handles.bat_scale_en		= param_find("MC_BAT_SCALE_EN");

    _params_handles.use_indi        =   param_find("MC_USE_INDI");
    _params_handles.use_indi_adaptive        =   param_find("MC_INDI_ADAPTIVE");
    _params_handles.indi_adaptive_mu        =   param_find("MC_INDI_MU");
    _params_handles.g1_p        =   param_find("MC_INDI_G1_P");
    _params_handles.g1_q        =   param_find("MC_INDI_G1_Q");
    _params_handles.g1_r        =   param_find("MC_INDI_G1_R");
    _params_handles.g2_r        =   param_find("MC_INDI_G2_R");
    _params_handles.ref_err_p        =   param_find("MC_INDI_ERROR_P");
    _params_handles.ref_err_q        =   param_find("MC_INDI_ERROR_Q");
    _params_handles.ref_err_r        =   param_find("MC_INDI_ERROR_R");
    _params_handles.ref_rate_p        =   param_find("MC_INDI_RATE_P");
    _params_handles.ref_rate_q        =   param_find("MC_INDI_RATE_Q");
    _params_handles.ref_rate_r        =   param_find("MC_INDI_RATE_R");
    _params_handles.act_dyn_p        =   param_find("MC_INDI_ACT_P");
    _params_handles.act_dyn_q        =   param_find("MC_INDI_ACT_Q");
    _params_handles.act_dyn_r        =   param_find("MC_INDI_ACT_R");
	/* rotations */
	_params_handles.board_rotation = param_find("SENS_BOARD_ROT");

	/* rotation offsets */
	_params_handles.board_offset[0] = param_find("SENS_BOARD_X_OFF");
	_params_handles.board_offset[1] = param_find("SENS_BOARD_Y_OFF");
	_params_handles.board_offset[2] = param_find("SENS_BOARD_Z_OFF");

	/* fetch initial parameter values */
	parameters_update();

    //INDI
    indi.max_rate = STABILIZATION_INDI_MAX_RATE;
    indi.attitude_max_yaw_rate = STABILIZATION_INDI_MAX_R;

    indi.g1 = {_params.g1_p, _params.g1_q, _params.g1_r};
    indi.g2 = _params.g2_r;
    /* Estimation parameters for adaptive INDI */
    indi.est.g1 = {
        _params.g1_p / (float)INDI_EST_SCALE,
        _params.g1_q / (float)INDI_EST_SCALE,
        _params.g1_r / (float)INDI_EST_SCALE
    };
    indi.est.g2 = _params.g2_r / (float)INDI_EST_SCALE;
    //INDI end

    stabilization_indi_enter();

	if (_params.vtol_type == 0 && _params.vtol_opt_recovery_enabled) {
		// the vehicle is a tailsitter, use optimal recovery control strategy
		_ts_opt_recovery = new TailsitterRecovery();
	}

	/* initialize thermal corrections as we might not immediately get a topic update (only non-zero values) */
	for (unsigned i = 0; i < 3; i++) {
		// used scale factors to unity
		_sensor_correction.gyro_scale_0[i] = 1.0f;
		_sensor_correction.gyro_scale_1[i] = 1.0f;
		_sensor_correction.gyro_scale_2[i] = 1.0f;
	}

}


MulticopterAttitudeControl::~MulticopterAttitudeControl()
{
	if (_control_task != -1) {
		/* task wakes up every 100ms or so at the longest */
		_task_should_exit = true;

		/* wait for a second for the task to quit at our request */
		unsigned i = 0;

		do {
			/* wait 20ms */
			usleep(20000);

			/* if we have given up, kill it */
			if (++i > 50) {
				px4_task_delete(_control_task);
				break;
			}
		} while (_control_task != -1);
	}

	if (_ts_opt_recovery != nullptr) {
		delete _ts_opt_recovery;
	}

	mc_att_control::g_control = nullptr;
}

int
MulticopterAttitudeControl::parameters_update()
{
	float v;

	float roll_tc, pitch_tc;

	param_get(_params_handles.roll_tc, &roll_tc);
	param_get(_params_handles.pitch_tc, &pitch_tc);

	/* roll gains */
	param_get(_params_handles.roll_p, &v);
	_params.att_p(0) = v * (ATTITUDE_TC_DEFAULT / roll_tc);
	param_get(_params_handles.roll_rate_p, &v);
	_params.rate_p(0) = v * (ATTITUDE_TC_DEFAULT / roll_tc);
	param_get(_params_handles.roll_rate_i, &v);
	_params.rate_i(0) = v;
	param_get(_params_handles.roll_rate_integ_lim, &v);
	_params.rate_int_lim(0) = v;
	param_get(_params_handles.roll_rate_d, &v);
	_params.rate_d(0) = v * (ATTITUDE_TC_DEFAULT / roll_tc);
	param_get(_params_handles.roll_rate_ff, &v);
	_params.rate_ff(0) = v;

	/* pitch gains */
	param_get(_params_handles.pitch_p, &v);
	_params.att_p(1) = v * (ATTITUDE_TC_DEFAULT / pitch_tc);
	param_get(_params_handles.pitch_rate_p, &v);
	_params.rate_p(1) = v * (ATTITUDE_TC_DEFAULT / pitch_tc);
	param_get(_params_handles.pitch_rate_i, &v);
	_params.rate_i(1) = v;
	param_get(_params_handles.pitch_rate_integ_lim, &v);
	_params.rate_int_lim(1) = v;
	param_get(_params_handles.pitch_rate_d, &v);
	_params.rate_d(1) = v * (ATTITUDE_TC_DEFAULT / pitch_tc);
	param_get(_params_handles.pitch_rate_ff, &v);
	_params.rate_ff(1) = v;

	param_get(_params_handles.tpa_breakpoint_p, &_params.tpa_breakpoint_p);
	param_get(_params_handles.tpa_breakpoint_i, &_params.tpa_breakpoint_i);
	param_get(_params_handles.tpa_breakpoint_d, &_params.tpa_breakpoint_d);
	param_get(_params_handles.tpa_rate_p, &_params.tpa_rate_p);
	param_get(_params_handles.tpa_rate_i, &_params.tpa_rate_i);
	param_get(_params_handles.tpa_rate_d, &_params.tpa_rate_d);

	/* yaw gains */
	param_get(_params_handles.yaw_p, &v);
	_params.att_p(2) = v;
	param_get(_params_handles.yaw_rate_p, &v);
	_params.rate_p(2) = v;
	param_get(_params_handles.yaw_rate_i, &v);
	_params.rate_i(2) = v;
	param_get(_params_handles.yaw_rate_integ_lim, &v);
	_params.rate_int_lim(2) = v;
	param_get(_params_handles.yaw_rate_d, &v);
	_params.rate_d(2) = v;
	param_get(_params_handles.yaw_rate_ff, &v);
	_params.rate_ff(2) = v;

	param_get(_params_handles.yaw_ff, &_params.yaw_ff);

	/* angular rate limits */
	param_get(_params_handles.roll_rate_max, &_params.roll_rate_max);
	_params.mc_rate_max(0) = math::radians(_params.roll_rate_max);
	param_get(_params_handles.pitch_rate_max, &_params.pitch_rate_max);
	_params.mc_rate_max(1) = math::radians(_params.pitch_rate_max);
	param_get(_params_handles.yaw_rate_max, &_params.yaw_rate_max);
	_params.mc_rate_max(2) = math::radians(_params.yaw_rate_max);

	/* auto angular rate limits */
	param_get(_params_handles.roll_rate_max, &_params.roll_rate_max);
	_params.auto_rate_max(0) = math::radians(_params.roll_rate_max);
	param_get(_params_handles.pitch_rate_max, &_params.pitch_rate_max);
	_params.auto_rate_max(1) = math::radians(_params.pitch_rate_max);
	param_get(_params_handles.yaw_auto_max, &_params.yaw_auto_max);
	_params.auto_rate_max(2) = math::radians(_params.yaw_auto_max);

	/* manual rate control scale and auto mode roll/pitch rate limits */
	param_get(_params_handles.acro_roll_max, &v);
	_params.acro_rate_max(0) = math::radians(v);
	param_get(_params_handles.acro_pitch_max, &v);
	_params.acro_rate_max(1) = math::radians(v);
	param_get(_params_handles.acro_yaw_max, &v);
	_params.acro_rate_max(2) = math::radians(v);

	/* stick deflection needed in rattitude mode to control rates not angles */
	param_get(_params_handles.rattitude_thres, &_params.rattitude_thres);

	param_get(_params_handles.vtol_type, &_params.vtol_type);

	int tmp;
	param_get(_params_handles.vtol_opt_recovery_enabled, &tmp);
	_params.vtol_opt_recovery_enabled = (bool)tmp;

	param_get(_params_handles.vtol_wv_yaw_rate_scale, &_params.vtol_wv_yaw_rate_scale);

	param_get(_params_handles.bat_scale_en, &_params.bat_scale_en);

	_actuators_0_circuit_breaker_enabled = circuit_breaker_enabled("CBRK_RATE_CTRL", CBRK_RATE_CTRL_KEY);

	/* rotation of the autopilot relative to the body */
	param_get(_params_handles.board_rotation, &(_params.board_rotation));

	/* fine adjustment of the rotation */
	param_get(_params_handles.board_offset[0], &(_params.board_offset[0]));
	param_get(_params_handles.board_offset[1], &(_params.board_offset[1]));
	param_get(_params_handles.board_offset[2], &(_params.board_offset[2]));
    /* INDI */
    param_get(_params_handles.use_indi,&(_params.use_indi));
    param_get(_params_handles.use_indi_adaptive,&(_params.use_indi_adaptive));
    param_get(_params_handles.indi_adaptive_mu,&(_params.indi_adaptive_mu));
    param_get(_params_handles.g1_p,&(_params.g1_p));
    param_get(_params_handles.g1_q,&(_params.g1_q));
    param_get(_params_handles.g1_r,&(_params.g1_r));
    param_get(_params_handles.g2_r,&(_params.g2_r));
    param_get(_params_handles.ref_err_p,&(_params.ref_err_p));
    param_get(_params_handles.ref_err_q,&(_params.ref_err_q));
    param_get(_params_handles.ref_err_r,&(_params.ref_err_r));
    param_get(_params_handles.ref_rate_p,&(_params.ref_rate_p));
    param_get(_params_handles.ref_rate_q,&(_params.ref_rate_q));
    param_get(_params_handles.ref_rate_r,&(_params.ref_rate_r));
    param_get(_params_handles.act_dyn_p,&(_params.act_dyn_p));
    param_get(_params_handles.act_dyn_q,&(_params.act_dyn_q));
    param_get(_params_handles.act_dyn_r,&(_params.act_dyn_r));
    //INDI

    indi.adaptive=(_params.use_indi_adaptive == 1);

    indi.reference_acceleration = {
      _params.ref_err_p,
      _params.ref_err_q,
      _params.ref_err_r,
      _params.ref_rate_p,
      _params.ref_rate_q,
      _params.ref_rate_r};

    indi.est.mu = _params.indi_adaptive_mu;
    //INDI end
	return OK;
}

void
MulticopterAttitudeControl::parameter_update_poll()
{
	bool updated;

	/* Check if parameters have changed */
	orb_check(_params_sub, &updated);

	if (updated) {
		struct parameter_update_s param_update;
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_update);
		parameters_update();
	}
}

void
MulticopterAttitudeControl::vehicle_control_mode_poll()
{
	bool updated;

	/* Check if vehicle control mode has changed */
	orb_check(_v_control_mode_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_control_mode), _v_control_mode_sub, &_v_control_mode);
	}
}

void
MulticopterAttitudeControl::vehicle_manual_poll()
{
	bool updated;

	/* get pilots inputs */
	orb_check(_manual_control_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(manual_control_setpoint), _manual_control_sp_sub, &_manual_control_sp);
	}
}

void
MulticopterAttitudeControl::vehicle_attitude_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_att_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_attitude_setpoint), _v_att_sp_sub, &_v_att_sp);
	}
}

void
MulticopterAttitudeControl::vehicle_rates_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_rates_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_rates_setpoint), _v_rates_sp_sub, &_v_rates_sp);
	}
}

void
MulticopterAttitudeControl::arming_status_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_armed_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(actuator_armed), _armed_sub, &_armed);
	}
}

void
MulticopterAttitudeControl::vehicle_status_poll()
{
	/* check if there is new status information */
	bool vehicle_status_updated;
	orb_check(_vehicle_status_sub, &vehicle_status_updated);

	if (vehicle_status_updated) {
		orb_copy(ORB_ID(vehicle_status), _vehicle_status_sub, &_vehicle_status);

		/* set correct uORB ID, depending on if vehicle is VTOL or not */
		if (!_rates_sp_id) {
			if (_vehicle_status.is_vtol) {
				_rates_sp_id = ORB_ID(mc_virtual_rates_setpoint);
				_actuators_id = ORB_ID(actuator_controls_virtual_mc);

			} else {
				_rates_sp_id = ORB_ID(vehicle_rates_setpoint);
				_actuators_id = ORB_ID(actuator_controls_0);
			}
		}
	}
}

void
MulticopterAttitudeControl::vehicle_motor_limits_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_motor_limits_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(multirotor_motor_limits), _motor_limits_sub, &_motor_limits);
		_saturation_status.value = _motor_limits.saturation_status;
	}
}

void
MulticopterAttitudeControl::battery_status_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_battery_status_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(battery_status), _battery_status_sub, &_battery_status);
	}
}

void
MulticopterAttitudeControl::control_state_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_ctrl_state_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(control_state), _ctrl_state_sub, &_ctrl_state);
	}
}

void
MulticopterAttitudeControl::sensor_correction_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_correction_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_correction), _sensor_correction_sub, &_sensor_correction);
	}

	/* update the latest gyro selection */
	if (_sensor_correction.selected_gyro_instance < _gyro_count) {
		_selected_gyro = _sensor_correction.selected_gyro_instance;
	}
}

/**
 * Function that initializes important values upon engaging INDI
 */
void
MulticopterAttitudeControl::stabilization_indi_init(void)
{
    // Initialize filters
    indi_init_filters();
}

/**
 * Function that resets important values upon engaging INDI.
 *
 * Don't reset inputs and filters, because it is unlikely to switch stabilization in flight,
 * and there are multiple modes that use (the same) stabilization. Resetting the controller
 * is not so nice when you are flying.
 * FIXME: Ideally we should detect when coming from something that is not INDI
 */
void
MulticopterAttitudeControl::stabilization_indi_enter(void)
{
  /* reset psi setpoint to current psi angle */
  //stab_att_sp_euler.psi = stabilization_attitude_get_heading_i();

  FLOAT_RATES_ZERO(indi.angular_accel_ref);
  FLOAT_RATES_ZERO(indi.u_act_dyn);
  FLOAT_RATES_ZERO(indi.u_in);

  // Re-initialize filters
  indi_init_filters();
}
/**
 * Function that resets the filters to zeros
 */
void
MulticopterAttitudeControl::indi_init_filters(void) {
    // tau = 1/(2*pi*Fc)
    float tau = 1.0/(2.0*M_PI*STABILIZATION_INDI_FILT_CUTOFF);
    float tau_r = 1.0/(2.0*M_PI*STABILIZATION_INDI_FILT_CUTOFF_R);
    float tau_axis[3] = {tau, tau, tau_r};
    float tau_est = 1.0/(2.0*M_PI*STABILIZATION_INDI_ESTIMATION_FILT_CUTOFF);
    float sample_time = 1.0/PERIODIC_FREQUENCY;
    // Filtering of gyroscope and actuators
    for(int8_t i=0; i<3; i++) {
      init_butterworth_2_low_pass(&indi.u[i], tau_axis[i], sample_time, 0.0);
      init_butterworth_2_low_pass(&indi.rate[i], tau_axis[i], sample_time, 0.0);
      init_butterworth_2_low_pass(&indi.est.u[i], tau_est, sample_time, 0.0);
      init_butterworth_2_low_pass(&indi.est.rate[i], tau_est, sample_time, 0.0);
    }
  }
/**
 * @brief Update butterworth filter for p, q and r of a FloatRates struct
 *
 * @param filter The filter array to use
 * @param new_values The new values
 */
void
MulticopterAttitudeControl::filter_pqr(Butterworth2LowPass *filter, struct FloatRates *new_values) {
  update_butterworth_2_low_pass(&filter[0], new_values->p);
  update_butterworth_2_low_pass(&filter[1], new_values->q);
  update_butterworth_2_low_pass(&filter[2], new_values->r);
}

/**
 * @brief Caclulate finite difference form a filter array
 * The filter already contains the previous values
 *
 * @param output The output array
 * @param filter The filter array input
 */
void
MulticopterAttitudeControl::finite_difference_from_filter(float *output, Butterworth2LowPass *filter,float dt) {
  for(int8_t i=0; i<3; i++) {
    output[i] = (filter[i].o[0] - filter[i].o[1])*(float)PERIODIC_FREQUENCY;
 //     output[i] = (filter[i].o[0] - filter[i].o[1]) / dt;

  }
}

/**
 * @brief Calculate derivative of an array via finite difference
 *
 * @param output[3] The output array
 * @param new[3] The newest values
 * @param old[3] The values of the previous timestep
 */
void
MulticopterAttitudeControl::finite_difference(float output[3], float new1[3], float old[3],float dt) {
  for(int8_t i=0; i<3; i++) {
    output[i] = (new1[i] - old[i])*(float)PERIODIC_FREQUENCY;
    //  output[i] = (new1[i] - old[i]) / dt;

  }
}

/**
 * This is a Least Mean Squares adaptive filter
 * It estimates the actuator effectiveness online, by comparing the expected
 * angular acceleration based on the inputs with the measured angular
 * acceleration
 */
void
MulticopterAttitudeControl::lms_estimation(float dt)
{
  static struct IndiEstimation *est = &indi.est;
  // Only pass really low frequencies so you don't adapt to noise
  //================姿态角速率,测量值===========================
  math::Vector<3> rates;
  get_body_rates(&rates);
  struct FloatRates body_rates;
  body_rates.p = rates(0);
  body_rates.q = rates(1);
  body_rates.r = rates(2);

  filter_pqr(est->u, &indi.u_act_dyn);
  filter_pqr(est->rate, &body_rates);

  // Calculate the first and second derivatives of the rates and actuators
  float rate_d_prev[3];
  float u_d_prev[3];
  float_vect_copy(rate_d_prev, est->rate_d, 3);
  float_vect_copy(u_d_prev, est->u_d, 3);
  finite_difference_from_filter(est->rate_d, est->rate,dt);
  finite_difference_from_filter(est->u_d, est->u,dt);
  finite_difference(est->rate_dd, est->rate_d, rate_d_prev,dt);
  finite_difference(est->u_dd, est->u_d, u_d_prev,dt);

  // The inputs are scaled in order to avoid overflows
  float du[3];
  float_vect_copy(du, est->u_d, 3);
  float_vect_scale(du, INDI_EST_SCALE, 3);
  est->g1.p = est->g1.p - (est->g1.p * du[0] - est->rate_dd[0]) * du[0] * est->mu;
  est->g1.q = est->g1.q - (est->g1.q * du[1] - est->rate_dd[1]) * du[1] * est->mu;
  float ddu = est->u_dd[2] * (float)INDI_EST_SCALE / (float)PERIODIC_FREQUENCY;
  //float ddu = est->u_dd[2] * (float)INDI_EST_SCALE *dt;
  float error = (est->g1.r * du[2] + est->g2 * ddu - est->rate_dd[2]);
  est->g1.r = est->g1.r - error * du[2] * est->mu / 3;
  est->g2 = est->g2 - error * 1000 * ddu * est->mu / 3;

  //the g values should be larger than zero, otherwise there is positive feedback, the command will go to max and there is nothing to learn anymore...
  if (est->g1.p < 0.01f) { est->g1.p = 0.01f; }
  if (est->g1.q < 0.01f) { est->g1.q = 0.01f; }
  if (est->g1.r < 0.01f) { est->g1.r = 0.01f; }
  if (est->g2   < 0.01f) { est->g2 = 0.01f; }

  if (indi.adaptive) {
    //Commit the estimated G values and apply the scaling
    indi.g1.p = est->g1.p * (float)INDI_EST_SCALE;
    indi.g1.q = est->g1.q * (float)INDI_EST_SCALE;
    indi.g1.r = est->g1.r * (float)INDI_EST_SCALE;
    indi.g2   = est->g2 * (float)INDI_EST_SCALE;

//    warnx("G1:%f#%f#%f",(double)indi.g1.p,(double)indi.g1.q,(double)indi.g1.r);
//    warnx("G2:%f",(double)indi.g2);
    _indi_status.g1_p = indi.g1.p;
    _indi_status.g1_q = indi.g1.q;
    _indi_status.g1_r = indi.g1.r;
    _indi_status.g2_r = indi.g2;
    _indi_status.timestamp = hrt_absolute_time();
    /* publish indi status */
    if (_indi_adaptive_pub != nullptr) {
        orb_publish(ORB_ID(indi), _indi_adaptive_pub, &_indi_status);

    } else {
        _indi_adaptive_pub = orb_advertise(ORB_ID(indi), &_indi_status);
    }
  }
}


void
MulticopterAttitudeControl::auto_save_indi_param(){

    bool save=false;

    if(fabsf(_params.g1_p - indi.g1.p) > 0.0001f){
        param_set(_params_handles.g1_p,&(indi.g1.p));
        save = true;
    }
    if(fabsf(_params.g1_q - indi.g1.q) > 0.0001f){
        param_set(_params_handles.g1_q,&(indi.g1.q));
        save = true;
    }
    if(fabsf(_params.g1_r - indi.g1.r) > 0.00001f){
        param_set(_params_handles.g1_r,&(indi.g1.r));
        save = true;
    }
    if(fabsf(_params.g2_r - indi.g2) > 0.00001f){
        param_set(_params_handles.g2_r,&(indi.g2));
        save = true;
    }

    if(save){
        //param_save_default();
        //warnx("indi-save#g1_p:%8.4f#g1_q:%8.6f#g1_r:%8.6f#g2_r:%8.6f",(double)indi.g1.p,(double)indi.g1.q,(double)indi.g1.r,(double)indi.g2);
    }
}
/**
 * @brief Does the INDI calculations
 *
 * @param indi_commands[] Array of commands that the function will write to
 * @param att_err quaternion attitude error
 * @param rate_control rate control enabled, otherwise attitude control
 */
void
MulticopterAttitudeControl::stabilization_indi_calc_cmd(int32_t indi_commands[], math::Vector<3> att_err, bool rate_control,float dt)
{
  // Propagate the filter on the gyroscopes and actuators
    //================姿态角速率,测量值===========================
    math::Vector<3> rates;
    get_body_rates(&rates);
    struct FloatRates body_rates;
    body_rates.p = rates(0);
    body_rates.q = rates(1);
    body_rates.r = rates(2);
  filter_pqr(indi.u, &indi.u_act_dyn);
  filter_pqr(indi.rate, &body_rates);

  // Calculate the derivative of the rates
  finite_difference_from_filter(indi.rate_d, indi.rate,dt);

  //The rates used for feedback are by default the measured rates. If needed they can be filtered (see below)
  struct FloatRates rates_for_feedback;
  RATES_COPY(rates_for_feedback, body_rates);

  //If there is a lot of noise on the gyroscope, it might be good to use the filtered value for feedback.
  //Note that due to the delay, the PD controller can not be as aggressive.
#if STABILIZATION_INDI_FILTER_ROLL_RATE
  rates_for_feedback.p = indi.rate[0].o[0];
#endif
#if STABILIZATION_INDI_FILTER_PITCH_RATE
  rates_for_feedback.q = indi.rate[1].o[0];
#endif
#if STABILIZATION_INDI_FILTER_YAW_RATE
  rates_for_feedback.r = indi.rate[2].o[0];
#endif

  indi.angular_accel_ref.p = indi.reference_acceleration.err_p * att_err(0)
                             - indi.reference_acceleration.rate_p * rates_for_feedback.p;

  indi.angular_accel_ref.q = indi.reference_acceleration.err_q * att_err(1)
                             - indi.reference_acceleration.rate_q * rates_for_feedback.q;

  //This separates the P and D controller and lets you impose a maximum yaw rate.
  float rate_ref_r = indi.reference_acceleration.err_r * att_err(2)/indi.reference_acceleration.rate_r;
  BoundAbs(rate_ref_r, indi.attitude_max_yaw_rate);
  indi.angular_accel_ref.r = indi.reference_acceleration.rate_r * (rate_ref_r - rates_for_feedback.r);

  /* Check if we are running the rate controller and overwrite */
  if(rate_control) {
      //=============================遥控器直接控制角速度===========================
    indi.angular_accel_ref.p =  indi.reference_acceleration.rate_p * (_rates_sp(0) - body_rates.p);
    indi.angular_accel_ref.q =  indi.reference_acceleration.rate_q * (_rates_sp(1) - body_rates.q);
    indi.angular_accel_ref.r =  indi.reference_acceleration.rate_r * (_rates_sp(2) - body_rates.r);
  }
    //_rates_sp(0) = indi.angular_accel_ref.p;
    //_rates_sp(1) = indi.angular_accel_ref.q;
    //_rates_sp(2) = indi.angular_accel_ref.r;
  //Increment in angular acceleration requires increment in control input
  //G1 is the control effectiveness. In the yaw axis, we need something additional: G2.
  //It takes care of the angular acceleration caused by the change in rotation rate of the propellers
  //(they have significant inertia, see the paper mentioned in the header for more explanation)
  indi.du.p = 1.0f / indi.g1.p * (indi.angular_accel_ref.p - indi.rate_d[0]);
  indi.du.q = 1.0f / indi.g1.q * (indi.angular_accel_ref.q - indi.rate_d[1]);
  indi.du.r = 1.0f / (indi.g1.r + indi.g2) * (indi.angular_accel_ref.r - indi.rate_d[2] + indi.g2 * indi.du.r);

  /* update increment only if motors are providing enough thrust to be effective */
  if (_thrust_sp > MIN_TAKEOFF_THRUST) {
      for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
          //Check for
          //roll saturation
          if(i == AXIS_INDEX_ROLL){
              if(_saturation_status.flags.roll_pos){
                indi.du.p = math::min(indi.du.p, 0.0f);
                //warnx("r+");
              }
              if(_saturation_status.flags.roll_neg){
                indi.du.p = math::max(indi.du.p, 0.0f);
                //warnx("r-");
              }
          }else
          //pitch saturation
          if(i == AXIS_INDEX_PITCH){
              if(_saturation_status.flags.pitch_pos){
                indi.du.q = math::min(indi.du.q, 0.0f);
                //warnx("p+");
              }
              if(_saturation_status.flags.pitch_neg){
                indi.du.q = math::max(indi.du.q, 0.0f);
                //warnx("p-");
              }
          }else
           //yaw saturation
          if(i == AXIS_INDEX_YAW){
              if(_saturation_status.flags.yaw_pos){
                indi.du.r = math::min(indi.du.r, 0.0f);
                //warnx("y+");
              }
              if(_saturation_status.flags.yaw_neg){
                indi.du.r = math::max(indi.du.r, 0.0f);
                //warnx("y-");
              }
          }

        }
  }

  //add the increment to the total control input
  indi.u_in.p = indi.u[0].o[0] + indi.du.p;
  indi.u_in.q = indi.u[1].o[0] + indi.du.q;
  indi.u_in.r = indi.u[2].o[0] + indi.du.r;

  //bound the total control input
  Bound(indi.u_in.p, -4500, 4500);
  Bound(indi.u_in.q, -4500, 4500);
  Bound(indi.u_in.r, -4500, 4500);

  //Propagate input filters
  //first order actuator dynamics
  indi.u_act_dyn.p = indi.u_act_dyn.p + STABILIZATION_INDI_ACT_DYN_P * (indi.u_in.p - indi.u_act_dyn.p);
  indi.u_act_dyn.q = indi.u_act_dyn.q + STABILIZATION_INDI_ACT_DYN_Q * (indi.u_in.q - indi.u_act_dyn.q);
  indi.u_act_dyn.r = indi.u_act_dyn.r + STABILIZATION_INDI_ACT_DYN_R * (indi.u_in.r - indi.u_act_dyn.r);

  //Don't increment if thrust is off
  //TODO: this should be something more elegant, but without this the inputs
  //will increment to the maximum before even getting in the air.
    if (_thrust_sp < MIN_TAKEOFF_THRUST) {
    FLOAT_RATES_ZERO(indi.du);
    FLOAT_RATES_ZERO(indi.u_act_dyn);
    FLOAT_RATES_ZERO(indi.u_in);
  } else if(_armed.armed){
    // only run the estimation if the commands are not zero.
    lms_estimation(dt);
  }
  /*  INDI feedback */
  indi_commands[COMMAND_ROLL] = indi.u_in.p;
  indi_commands[COMMAND_PITCH] = indi.u_in.q;
  indi_commands[COMMAND_YAW] = indi.u_in.r;
}

/**
 * @brief runs stabilization indi
 *
 * @param in_flight not used
 * @param rate_control rate control enabled, otherwise attitude control
 */
void
MulticopterAttitudeControl::stabilization_indi_run(bool in_flight __attribute__((unused)), bool rate_control,float dt)
{
  /*姿态误差,四元数*/
  math::Vector<3> att_err;
  get_attitude_errors(&att_err);

  /* compute the INDI command */
  stabilization_indi_calc_cmd(stabilization_att_indi_cmd, att_err, rate_control,dt);


  /* copy the INDI command */
  _att_control(AXIS_INDEX_ROLL) = stabilization_att_indi_cmd[COMMAND_ROLL] / 4500.0f;
  _att_control(AXIS_INDEX_PITCH) = stabilization_att_indi_cmd[COMMAND_PITCH] / 4500.0f;
  _att_control(AXIS_INDEX_YAW) = stabilization_att_indi_cmd[COMMAND_YAW] / 4500.0f;

}






/**
 * Attitude controller.
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_rates_sp' vector, '_thrust_sp'
 */
void
MulticopterAttitudeControl::control_attitude(float dt)
{
	vehicle_attitude_setpoint_poll();

	_thrust_sp = _v_att_sp.thrust;

	/* construct attitude setpoint rotation matrix */
	math::Quaternion q_sp(_v_att_sp.q_d[0], _v_att_sp.q_d[1], _v_att_sp.q_d[2], _v_att_sp.q_d[3]);
	math::Matrix<3, 3> R_sp = q_sp.to_dcm();

	/* get current rotation matrix from control state quaternions */
	math::Quaternion q_att(_ctrl_state.q[0], _ctrl_state.q[1], _ctrl_state.q[2], _ctrl_state.q[3]);
	math::Matrix<3, 3> R = q_att.to_dcm();

	/* all input data is ready, run controller itself */

	/* try to move thrust vector shortest way, because yaw response is slower than roll/pitch */
	math::Vector<3> R_z(R(0, 2), R(1, 2), R(2, 2));
	math::Vector<3> R_sp_z(R_sp(0, 2), R_sp(1, 2), R_sp(2, 2));

	/* axis and sin(angle) of desired rotation */
	math::Vector<3> e_R = R.transposed() * (R_z % R_sp_z);

	/* calculate angle error */
	float e_R_z_sin = e_R.length();
	float e_R_z_cos = R_z * R_sp_z;

	/* calculate weight for yaw control */
	float yaw_w = R_sp(2, 2) * R_sp(2, 2);

	/* calculate rotation matrix after roll/pitch only rotation */
	math::Matrix<3, 3> R_rp;

	if (e_R_z_sin > 0.0f) {
		/* get axis-angle representation */
		float e_R_z_angle = atan2f(e_R_z_sin, e_R_z_cos);
		math::Vector<3> e_R_z_axis = e_R / e_R_z_sin;

		e_R = e_R_z_axis * e_R_z_angle;

		/* cross product matrix for e_R_axis */
		math::Matrix<3, 3> e_R_cp;
		e_R_cp.zero();
		e_R_cp(0, 1) = -e_R_z_axis(2);
		e_R_cp(0, 2) = e_R_z_axis(1);
		e_R_cp(1, 0) = e_R_z_axis(2);
		e_R_cp(1, 2) = -e_R_z_axis(0);
		e_R_cp(2, 0) = -e_R_z_axis(1);
		e_R_cp(2, 1) = e_R_z_axis(0);

		/* rotation matrix for roll/pitch only rotation */
		R_rp = R * (_I + e_R_cp * e_R_z_sin + e_R_cp * e_R_cp * (1.0f - e_R_z_cos));

	} else {
		/* zero roll/pitch rotation */
		R_rp = R;
	}

	/* R_rp and R_sp has the same Z axis, calculate yaw error */
	math::Vector<3> R_sp_x(R_sp(0, 0), R_sp(1, 0), R_sp(2, 0));
	math::Vector<3> R_rp_x(R_rp(0, 0), R_rp(1, 0), R_rp(2, 0));
	e_R(2) = atan2f((R_rp_x % R_sp_x) * R_sp_z, R_rp_x * R_sp_x) * yaw_w;

	if (e_R_z_cos < 0.0f) {
		/* for large thrust vector rotations use another rotation method:
		 * calculate angle and axis for R -> R_sp rotation directly */
		math::Quaternion q_error;
		q_error.from_dcm(R.transposed() * R_sp);
		math::Vector<3> e_R_d = q_error(0) >= 0.0f ? q_error.imag()  * 2.0f : -q_error.imag() * 2.0f;

		/* use fusion of Z axis based rotation and direct rotation */
		float direct_w = e_R_z_cos * e_R_z_cos * yaw_w;
		e_R = e_R * (1.0f - direct_w) + e_R_d * direct_w;
	}

	/* calculate angular rates setpoint */
	_rates_sp = _params.att_p.emult(e_R);

	/* limit rates */
	for (int i = 0; i < 3; i++) {
		if ((_v_control_mode.flag_control_velocity_enabled || _v_control_mode.flag_control_auto_enabled) &&
		    !_v_control_mode.flag_control_manual_enabled) {
			_rates_sp(i) = math::constrain(_rates_sp(i), -_params.auto_rate_max(i), _params.auto_rate_max(i));

		} else {
			_rates_sp(i) = math::constrain(_rates_sp(i), -_params.mc_rate_max(i), _params.mc_rate_max(i));
		}
	}

	/* feed forward yaw setpoint rate */
	_rates_sp(2) += _v_att_sp.yaw_sp_move_rate * yaw_w * _params.yaw_ff;

	/* weather-vane mode, dampen yaw rate */
	if ((_v_control_mode.flag_control_velocity_enabled || _v_control_mode.flag_control_auto_enabled) &&
	    _v_att_sp.disable_mc_yaw_control == true && !_v_control_mode.flag_control_manual_enabled) {
		float wv_yaw_rate_max = _params.auto_rate_max(2) * _params.vtol_wv_yaw_rate_scale;
		_rates_sp(2) = math::constrain(_rates_sp(2), -wv_yaw_rate_max, wv_yaw_rate_max);
		// prevent integrator winding up in weathervane mode
		_rates_int(2) = 0.0f;
	}
}

/*
 * Throttle PID attenuation
 * Function visualization available here https://www.desmos.com/calculator/gn4mfoddje
 * Input: 'tpa_breakpoint', 'tpa_rate', '_thrust_sp'
 * Output: 'pidAttenuationPerAxis' vector
 */
math::Vector<3>
MulticopterAttitudeControl::pid_attenuations(float tpa_breakpoint, float tpa_rate)
{
	/* throttle pid attenuation factor */
	float tpa = 1.0f - tpa_rate * (fabsf(_v_rates_sp.thrust) - tpa_breakpoint) / (1.0f - tpa_breakpoint);
	tpa = fmaxf(TPA_RATE_LOWER_LIMIT, fminf(1.0f, tpa));

	math::Vector<3> pidAttenuationPerAxis;
	pidAttenuationPerAxis(AXIS_INDEX_ROLL) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_PITCH) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_YAW) = 1.0;

	return pidAttenuationPerAxis;
}

void
MulticopterAttitudeControl::get_attitude_errors(math::Vector<3> *att_e)
{
    vehicle_attitude_setpoint_poll();

    _thrust_sp = _v_att_sp.thrust;

    /* construct attitude setpoint rotation matrix */
    math::Quaternion q_sp(_v_att_sp.q_d[0], _v_att_sp.q_d[1], _v_att_sp.q_d[2], _v_att_sp.q_d[3]);
    math::Matrix<3, 3> R_sp = q_sp.to_dcm();

    /* get current rotation matrix from control state quaternions */
    math::Quaternion q_att(_ctrl_state.q[0], _ctrl_state.q[1], _ctrl_state.q[2], _ctrl_state.q[3]);
    math::Matrix<3, 3> R = q_att.to_dcm();

    /* all input data is ready, run controller itself */

    /* try to move thrust vector shortest way, because yaw response is slower than roll/pitch */
    math::Vector<3> R_z(R(0, 2), R(1, 2), R(2, 2));
    math::Vector<3> R_sp_z(R_sp(0, 2), R_sp(1, 2), R_sp(2, 2));

    /* axis and sin(angle) of desired rotation */
    math::Vector<3> e_R = R.transposed() * (R_z % R_sp_z);

    /* calculate angle error */
    float e_R_z_sin = e_R.length();
    float e_R_z_cos = R_z * R_sp_z;

    /* calculate weight for yaw control */
    float yaw_w = R_sp(2, 2) * R_sp(2, 2);

    /* calculate rotation matrix after roll/pitch only rotation */
    math::Matrix<3, 3> R_rp;

    if (e_R_z_sin > 0.0f) {
        /* get axis-angle representation */
        float e_R_z_angle = atan2f(e_R_z_sin, e_R_z_cos);
        math::Vector<3> e_R_z_axis = e_R / e_R_z_sin;

        e_R = e_R_z_axis * e_R_z_angle;

        /* cross product matrix for e_R_axis */
        math::Matrix<3, 3> e_R_cp;
        e_R_cp.zero();
        e_R_cp(0, 1) = -e_R_z_axis(2);
        e_R_cp(0, 2) = e_R_z_axis(1);
        e_R_cp(1, 0) = e_R_z_axis(2);
        e_R_cp(1, 2) = -e_R_z_axis(0);
        e_R_cp(2, 0) = -e_R_z_axis(1);
        e_R_cp(2, 1) = e_R_z_axis(0);

        /* rotation matrix for roll/pitch only rotation */
        R_rp = R * (_I + e_R_cp * e_R_z_sin + e_R_cp * e_R_cp * (1.0f - e_R_z_cos));

    } else {
        /* zero roll/pitch rotation */
        R_rp = R;
    }

    /* R_rp and R_sp has the same Z axis, calculate yaw error */
    math::Vector<3> R_sp_x(R_sp(0, 0), R_sp(1, 0), R_sp(2, 0));
    math::Vector<3> R_rp_x(R_rp(0, 0), R_rp(1, 0), R_rp(2, 0));
    e_R(2) = atan2f((R_rp_x % R_sp_x) * R_sp_z, R_rp_x * R_sp_x) * yaw_w;

    if (e_R_z_cos < 0.0f) {
        /* for large thrust vector rotations use another rotation method:
         * calculate angle and axis for R -> R_sp rotation directly */
        math::Quaternion q_error;
        q_error.from_dcm(R.transposed() * R_sp);
        math::Vector<3> e_R_d = q_error(0) >= 0.0f ? q_error.imag()  * 2.0f : -q_error.imag() * 2.0f;

        /* use fusion of Z axis based rotation and direct rotation */
        float direct_w = e_R_z_cos * e_R_z_cos * yaw_w;
        e_R = e_R * (1.0f - direct_w) + e_R_d * direct_w;
    }

    *att_e = e_R;
}
/*
 * Output: 'body rates' vector
 */
void
MulticopterAttitudeControl::get_body_rates(math::Vector<3> *body_rates)
{
    math::Vector<3> rates;
    /* get transformation matrix from sensor/board to body frame */
    get_rot_matrix((enum Rotation)_params.board_rotation, &_board_rotation);

    /* fine tune the rotation */
    math::Matrix<3, 3> board_rotation_offset;
    board_rotation_offset.from_euler(M_DEG_TO_RAD_F * _params.board_offset[0],
                     M_DEG_TO_RAD_F * _params.board_offset[1],
                     M_DEG_TO_RAD_F * _params.board_offset[2]);
    _board_rotation = board_rotation_offset * _board_rotation;

    // get the raw gyro data and correct for thermal errors

    if (_selected_gyro == 0) {
        rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_0[0]) * _sensor_correction.gyro_scale_0[0];
        rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_0[1]) * _sensor_correction.gyro_scale_0[1];
        rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_0[2]) * _sensor_correction.gyro_scale_0[2];

    } else if (_selected_gyro == 1) {
        rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_1[0]) * _sensor_correction.gyro_scale_1[0];
        rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_1[1]) * _sensor_correction.gyro_scale_1[1];
        rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_1[2]) * _sensor_correction.gyro_scale_1[2];

    } else if (_selected_gyro == 2) {
        rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_2[0]) * _sensor_correction.gyro_scale_2[0];
        rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_2[1]) * _sensor_correction.gyro_scale_2[1];
        rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_2[2]) * _sensor_correction.gyro_scale_2[2];

    } else {
        rates(0) = _sensor_gyro.x;
        rates(1) = _sensor_gyro.y;
        rates(2) = _sensor_gyro.z;
    }

    // rotate corrected measurements from sensor to body frame
    rates = _board_rotation * rates;

    // correct for in-run bias errors
    rates(0) -= _ctrl_state.roll_rate_bias;
    rates(1) -= _ctrl_state.pitch_rate_bias;
    rates(2) -= _ctrl_state.yaw_rate_bias;
    *body_rates = rates;
}


/*
 * Attitude rates controller.
 * Input: '_rates_sp' vector, '_thrust_sp'
 * Output: '_att_control' vector
 */
void
MulticopterAttitudeControl::control_attitude_rates(float dt)
{
	/* reset integral if disarmed */
	if (!_armed.armed || !_vehicle_status.is_rotary_wing) {
		_rates_int.zero();
	}

	/* get transformation matrix from sensor/board to body frame */
	get_rot_matrix((enum Rotation)_params.board_rotation, &_board_rotation);

	/* fine tune the rotation */
	math::Matrix<3, 3> board_rotation_offset;
	board_rotation_offset.from_euler(M_DEG_TO_RAD_F * _params.board_offset[0],
					 M_DEG_TO_RAD_F * _params.board_offset[1],
					 M_DEG_TO_RAD_F * _params.board_offset[2]);
	_board_rotation = board_rotation_offset * _board_rotation;

	// get the raw gyro data and correct for thermal errors
	math::Vector<3> rates;

	if (_selected_gyro == 0) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_0[0]) * _sensor_correction.gyro_scale_0[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_0[1]) * _sensor_correction.gyro_scale_0[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_0[2]) * _sensor_correction.gyro_scale_0[2];

	} else if (_selected_gyro == 1) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_1[0]) * _sensor_correction.gyro_scale_1[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_1[1]) * _sensor_correction.gyro_scale_1[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_1[2]) * _sensor_correction.gyro_scale_1[2];

	} else if (_selected_gyro == 2) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_2[0]) * _sensor_correction.gyro_scale_2[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_2[1]) * _sensor_correction.gyro_scale_2[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_2[2]) * _sensor_correction.gyro_scale_2[2];

	} else {
		rates(0) = _sensor_gyro.x;
		rates(1) = _sensor_gyro.y;
		rates(2) = _sensor_gyro.z;
	}

	// rotate corrected measurements from sensor to body frame
	rates = _board_rotation * rates;

	// correct for in-run bias errors
	rates(0) -= _ctrl_state.roll_rate_bias;
	rates(1) -= _ctrl_state.pitch_rate_bias;
	rates(2) -= _ctrl_state.yaw_rate_bias;
    //
	math::Vector<3> rates_p_scaled = _params.rate_p.emult(pid_attenuations(_params.tpa_breakpoint_p, _params.tpa_rate_p));
	math::Vector<3> rates_i_scaled = _params.rate_i.emult(pid_attenuations(_params.tpa_breakpoint_i, _params.tpa_rate_i));
	math::Vector<3> rates_d_scaled = _params.rate_d.emult(pid_attenuations(_params.tpa_breakpoint_d, _params.tpa_rate_d));

	/* angular rates error */
	math::Vector<3> rates_err = _rates_sp - rates;

	_att_control = rates_p_scaled.emult(rates_err) +
		       _rates_int +
		       rates_d_scaled.emult(_rates_prev - rates) / dt +
		       _params.rate_ff.emult(_rates_sp);

	_rates_sp_prev = _rates_sp;
	_rates_prev = rates;

	/* update integral only if motors are providing enough thrust to be effective */
	if (_thrust_sp > MIN_TAKEOFF_THRUST) {
		for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
			// Check for positive control saturation
			bool positive_saturation =
				((i == AXIS_INDEX_ROLL) && _saturation_status.flags.roll_pos) ||
				((i == AXIS_INDEX_PITCH) && _saturation_status.flags.pitch_pos) ||
				((i == AXIS_INDEX_YAW) && _saturation_status.flags.yaw_pos);

			// Check for negative control saturation
			bool negative_saturation =
				((i == AXIS_INDEX_ROLL) && _saturation_status.flags.roll_neg) ||
				((i == AXIS_INDEX_PITCH) && _saturation_status.flags.pitch_neg) ||
				((i == AXIS_INDEX_YAW) && _saturation_status.flags.yaw_neg);

			// prevent further positive control saturation
			if (positive_saturation) {
				rates_err(i) = math::min(rates_err(i), 0.0f);

			}

			// prevent further negative control saturation
			if (negative_saturation) {
				rates_err(i) = math::max(rates_err(i), 0.0f);

            }

			// Perform the integration using a first order method and do not propaate the result if out of range or invalid
			float rate_i = _rates_int(i) + _params.rate_i(i) * rates_err(i) * dt;

			if (PX4_ISFINITE(rate_i) && rate_i > -_params.rate_int_lim(i) && rate_i < _params.rate_int_lim(i)) {
				_rates_int(i) = rate_i;

			}
		}
	}

	/* explicitly limit the integrator state */
	for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
		_rates_int(i) = math::constrain(_rates_int(i), -_params.rate_int_lim(i), _params.rate_int_lim(i));

	}
}

void
MulticopterAttitudeControl::task_main_trampoline(int argc, char *argv[])
{
	mc_att_control::g_control->task_main();
}

void
MulticopterAttitudeControl::task_main()
{

	/*
	 * do subscriptions
	 */
	_v_att_sp_sub = orb_subscribe(ORB_ID(vehicle_attitude_setpoint));
	_v_rates_sp_sub = orb_subscribe(ORB_ID(vehicle_rates_setpoint));
	_ctrl_state_sub = orb_subscribe(ORB_ID(control_state));
	_v_control_mode_sub = orb_subscribe(ORB_ID(vehicle_control_mode));
	_params_sub = orb_subscribe(ORB_ID(parameter_update));
	_manual_control_sp_sub = orb_subscribe(ORB_ID(manual_control_setpoint));
	_armed_sub = orb_subscribe(ORB_ID(actuator_armed));
	_vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));
	_motor_limits_sub = orb_subscribe(ORB_ID(multirotor_motor_limits));
	_battery_status_sub = orb_subscribe(ORB_ID(battery_status));

	_gyro_count = math::min(orb_group_count(ORB_ID(sensor_gyro)), MAX_GYRO_COUNT);

	if (_gyro_count == 0) {
		_gyro_count = 1;
	}

	for (unsigned s = 0; s < _gyro_count; s++) {
		_sensor_gyro_sub[s] = orb_subscribe_multi(ORB_ID(sensor_gyro), s);
	}

	_sensor_correction_sub = orb_subscribe(ORB_ID(sensor_correction));

	/* initialize parameters cache */
	parameters_update();

	/* wakeup source: gyro data from sensor selected by the sensor app */
	px4_pollfd_struct_t poll_fds = {};
	poll_fds.events = POLLIN;

	while (!_task_should_exit) {

		poll_fds.fd = _sensor_gyro_sub[_selected_gyro];

		/* wait for up to 100ms for data */
		int pret = px4_poll(&poll_fds, 1, 100);

		/* timed out - periodic check for _task_should_exit */
		if (pret == 0) {
			continue;
		}

		/* this is undesirable but not much we can do - might want to flag unhappy status */
		if (pret < 0) {
			warn("mc att ctrl: poll error %d, %d", pret, errno);
			/* sleep a bit before next try */
			usleep(100000);
			continue;
		}

		perf_begin(_loop_perf);

		/* run controller on gyro changes */
		if (poll_fds.revents & POLLIN) {
			static uint64_t last_run = 0;
            static uint64_t last_save = 0;
			float dt = (hrt_absolute_time() - last_run) / 1000000.0f;
			last_run = hrt_absolute_time();

			/* guard against too small (< 2ms) and too large (> 20ms) dt's */
			if (dt < 0.002f) {
				dt = 0.002f;

			} else if (dt > 0.02f) {
				dt = 0.02f;
			}

			/* copy gyro data */
			orb_copy(ORB_ID(sensor_gyro), _sensor_gyro_sub[_selected_gyro], &_sensor_gyro);

			/* check for updates in other topics */
			parameter_update_poll();
			vehicle_control_mode_poll();
			arming_status_poll();
			vehicle_manual_poll();
			vehicle_status_poll();
			vehicle_motor_limits_poll();
			battery_status_poll();
			control_state_poll();
			sensor_correction_poll();

			/* Check if we are in rattitude mode and the pilot is above the threshold on pitch
			 * or roll (yaw can rotate 360 in normal att control).  If both are true don't
			 * even bother running the attitude controllers */
			if (_v_control_mode.flag_control_rattitude_enabled) {
				if (fabsf(_manual_control_sp.y) > _params.rattitude_thres ||
				    fabsf(_manual_control_sp.x) > _params.rattitude_thres) {
					_v_control_mode.flag_control_attitude_enabled = false;
				}
			}

			if (_v_control_mode.flag_control_attitude_enabled) {

				if (_ts_opt_recovery == nullptr) {
					// the  tailsitter recovery instance has not been created, thus, the vehicle
					// is not a tailsitter, do normal attitude control
                   if(!_params.use_indi)
                    control_attitude(dt);


				} else {
					vehicle_attitude_setpoint_poll();
					_thrust_sp = _v_att_sp.thrust;
					math::Quaternion q(_ctrl_state.q[0], _ctrl_state.q[1], _ctrl_state.q[2], _ctrl_state.q[3]);
					math::Quaternion q_sp(&_v_att_sp.q_d[0]);
					_ts_opt_recovery->setAttGains(_params.att_p, _params.yaw_ff);
					_ts_opt_recovery->calcOptimalRates(q, q_sp, _v_att_sp.yaw_sp_move_rate, _rates_sp);

					/* limit rates */
					for (int i = 0; i < 3; i++) {
						_rates_sp(i) = math::constrain(_rates_sp(i), -_params.mc_rate_max(i), _params.mc_rate_max(i));
					}
				}

				/* publish attitude rates setpoint */
				_v_rates_sp.roll = _rates_sp(0);
				_v_rates_sp.pitch = _rates_sp(1);
				_v_rates_sp.yaw = _rates_sp(2);
				_v_rates_sp.thrust = _thrust_sp;
				_v_rates_sp.timestamp = hrt_absolute_time();

				if (_v_rates_sp_pub != nullptr) {
					orb_publish(_rates_sp_id, _v_rates_sp_pub, &_v_rates_sp);

				} else if (_rates_sp_id) {
					_v_rates_sp_pub = orb_advertise(_rates_sp_id, &_v_rates_sp);
				}

				//}

			} else {
				/* attitude controller disabled, poll rates setpoint topic */
				if (_v_control_mode.flag_control_manual_enabled) {
					/* manual rates control - ACRO mode */
					_rates_sp = math::Vector<3>(_manual_control_sp.y, -_manual_control_sp.x,
								    _manual_control_sp.r).emult(_params.acro_rate_max);
					_thrust_sp = math::min(_manual_control_sp.z, MANUAL_THROTTLE_MAX_MULTICOPTER);

					/* publish attitude rates setpoint */
					_v_rates_sp.roll = _rates_sp(0);
					_v_rates_sp.pitch = _rates_sp(1);
					_v_rates_sp.yaw = _rates_sp(2);
					_v_rates_sp.thrust = _thrust_sp;
					_v_rates_sp.timestamp = hrt_absolute_time();

					if (_v_rates_sp_pub != nullptr) {
						orb_publish(_rates_sp_id, _v_rates_sp_pub, &_v_rates_sp);

					} else if (_rates_sp_id) {
						_v_rates_sp_pub = orb_advertise(_rates_sp_id, &_v_rates_sp);
					}

				} else {
					/* attitude controller disabled, poll rates setpoint topic */
					vehicle_rates_setpoint_poll();
					_rates_sp(0) = _v_rates_sp.roll;
					_rates_sp(1) = _v_rates_sp.pitch;
					_rates_sp(2) = _v_rates_sp.yaw;
					_thrust_sp = _v_rates_sp.thrust;
				}
			}

			if (_v_control_mode.flag_control_rates_enabled) {
               if(_params.use_indi){
                    stabilization_indi_run(false,false,dt);
                    //定时保存参数
                    if(hrt_absolute_time() - last_save > 2000000.0f && _armed.armed){

                        auto_save_indi_param();

                        last_save = hrt_absolute_time();
                    }
               }
                else
                    control_attitude_rates(dt);

				/* publish actuator controls */
				_actuators.control[0] = (PX4_ISFINITE(_att_control(0))) ? _att_control(0) : 0.0f;
				_actuators.control[1] = (PX4_ISFINITE(_att_control(1))) ? _att_control(1) : 0.0f;
				_actuators.control[2] = (PX4_ISFINITE(_att_control(2))) ? _att_control(2) : 0.0f;
				_actuators.control[3] = (PX4_ISFINITE(_thrust_sp)) ? _thrust_sp : 0.0f;
				_actuators.control[7] = _v_att_sp.landing_gear;
				_actuators.timestamp = hrt_absolute_time();
				_actuators.timestamp_sample = _ctrl_state.timestamp;

				/* scale effort by battery status */
				if (_params.bat_scale_en && _battery_status.scale > 0.0f) {
					for (int i = 0; i < 4; i++) {
						_actuators.control[i] *= _battery_status.scale;
					}
				}

				_controller_status.roll_rate_integ = _rates_int(0);
				_controller_status.pitch_rate_integ = _rates_int(1);
				_controller_status.yaw_rate_integ = _rates_int(2);
				_controller_status.timestamp = hrt_absolute_time();

				if (!_actuators_0_circuit_breaker_enabled) {
					if (_actuators_0_pub != nullptr) {

						orb_publish(_actuators_id, _actuators_0_pub, &_actuators);
						perf_end(_controller_latency_perf);

					} else if (_actuators_id) {
						_actuators_0_pub = orb_advertise(_actuators_id, &_actuators);
					}

				}

				/* publish controller status */
				if (_controller_status_pub != nullptr) {
					orb_publish(ORB_ID(mc_att_ctrl_status), _controller_status_pub, &_controller_status);

				} else {
					_controller_status_pub = orb_advertise(ORB_ID(mc_att_ctrl_status), &_controller_status);
				}
			}

			if (_v_control_mode.flag_control_termination_enabled) {
				if (!_vehicle_status.is_vtol) {

					_rates_sp.zero();
					_rates_int.zero();
					_thrust_sp = 0.0f;
					_att_control.zero();


					/* publish actuator controls */
					_actuators.control[0] = 0.0f;
					_actuators.control[1] = 0.0f;
					_actuators.control[2] = 0.0f;
					_actuators.control[3] = 0.0f;
					_actuators.timestamp = hrt_absolute_time();
					_actuators.timestamp_sample = _ctrl_state.timestamp;

					if (!_actuators_0_circuit_breaker_enabled) {
						if (_actuators_0_pub != nullptr) {

							orb_publish(_actuators_id, _actuators_0_pub, &_actuators);
							perf_end(_controller_latency_perf);

						} else if (_actuators_id) {
							_actuators_0_pub = orb_advertise(_actuators_id, &_actuators);
						}
					}

					_controller_status.roll_rate_integ = _rates_int(0);
					_controller_status.pitch_rate_integ = _rates_int(1);
					_controller_status.yaw_rate_integ = _rates_int(2);
					_controller_status.timestamp = hrt_absolute_time();

					/* publish controller status */
					if (_controller_status_pub != nullptr) {
						orb_publish(ORB_ID(mc_att_ctrl_status), _controller_status_pub, &_controller_status);

					} else {
						_controller_status_pub = orb_advertise(ORB_ID(mc_att_ctrl_status), &_controller_status);
					}

					/* publish attitude rates setpoint */
					_v_rates_sp.roll = _rates_sp(0);
					_v_rates_sp.pitch = _rates_sp(1);
					_v_rates_sp.yaw = _rates_sp(2);
					_v_rates_sp.thrust = _thrust_sp;
					_v_rates_sp.timestamp = hrt_absolute_time();

					if (_v_rates_sp_pub != nullptr) {
						orb_publish(_rates_sp_id, _v_rates_sp_pub, &_v_rates_sp);

					} else if (_rates_sp_id) {
						_v_rates_sp_pub = orb_advertise(_rates_sp_id, &_v_rates_sp);
					}
				}
			}
		}

		perf_end(_loop_perf);
	}

	_control_task = -1;
}

int
MulticopterAttitudeControl::start()
{
	ASSERT(_control_task == -1);

	/* start the task */
	_control_task = px4_task_spawn_cmd("mc_att_control",
					   SCHED_DEFAULT,
					   SCHED_PRIORITY_MAX - 5,
					   1700,
					   (px4_main_t)&MulticopterAttitudeControl::task_main_trampoline,
					   nullptr);

	if (_control_task < 0) {
		warn("task start failed");
		return -errno;
	}

	return OK;
}

int mc_att_control_indi_main(int argc, char *argv[])
{
	if (argc < 2) {
        warnx("usage: mc_att_control {start|stop|status}");
		return 1;
	}

	if (!strcmp(argv[1], "start")) {

		if (mc_att_control::g_control != nullptr) {
			warnx("already running");
			return 1;
		}

		mc_att_control::g_control = new MulticopterAttitudeControl;

		if (mc_att_control::g_control == nullptr) {
			warnx("alloc failed");
			return 1;
		}

		if (OK != mc_att_control::g_control->start()) {
			delete mc_att_control::g_control;
			mc_att_control::g_control = nullptr;
			warnx("start failed");
			return 1;
		}

		return 0;
	}

	if (!strcmp(argv[1], "stop")) {
		if (mc_att_control::g_control == nullptr) {
			warnx("not running");
			return 1;
		}

		delete mc_att_control::g_control;
		mc_att_control::g_control = nullptr;
		return 0;
	}

	if (!strcmp(argv[1], "status")) {
		if (mc_att_control::g_control) {
			warnx("running");
			return 0;

		} else {
			warnx("not running");
			return 1;
		}
	}

	warnx("unrecognized command");
	return 1;
}
