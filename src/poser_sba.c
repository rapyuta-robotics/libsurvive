#ifndef USE_DOUBLE
#define FLT double
#define USE_DOUBLE
#endif

#include <malloc.h>
#include <sba/sba.h>

#include "poser.h"
#include <survive.h>
#include <survive_imu.h>

#include "assert.h"
#include "linmath.h"
#include "math.h"
#include "poser_general_optimizer.h"
#include "string.h"
#include "survive_cal.h"
#include "survive_config.h"
#include "survive_kalman.h"
#include "survive_reproject.h"

typedef struct {
	PoserData *pdfs;
	SurviveObject *so;
	SurvivePose obj_pose;
	SurvivePose camera_params[2];
} sba_context;

typedef struct SBAData {
	GeneralOptimizerData opt;

	int last_acode;
	int last_lh;

	FLT sensor_variance;
	FLT sensor_variance_per_second;
	int sensor_time_window;
	int use_jacobian_function;
	int required_meas;

	survive_kpose_t kpose;
	SurviveIMUTracker tracker;

	bool useIMU;

	struct {
		int meas_failures;
	} stats;
} SBAData;

static void metric_function(int j, int i, double *aj, double *xij, void *adata) {
	sba_context *ctx = (sba_context *)(adata);
	SurviveObject *so = ctx->so;

	SurvivePose obj2world = ctx->obj_pose;
	FLT sensorInWorld[3] = {0};
	ApplyPoseToPoint(sensorInWorld, &obj2world, &so->sensor_locations[i * 3]);
	survive_calibration_config cfg = so->ctx->calibration_config;
	survive_reproject_from_pose_with_config(so->ctx, &cfg, j, (SurvivePose *)aj, sensorInWorld, xij);
}

static size_t construct_input(const SurviveObject *so, PoserDataFullScene *pdfs, char *vmask, double *meas) {
	size_t measCount = 0;
	size_t size = so->sensor_ct * NUM_LIGHTHOUSES; // One set per lighthouse
	for (size_t sensor = 0; sensor < so->sensor_ct; sensor++) {
		for (size_t lh = 0; lh < 2; lh++) {
			FLT *l = pdfs->lengths[sensor][lh];
			if (l[0] < 0 || l[1] < 0) {
				vmask[sensor * NUM_LIGHTHOUSES + lh] = 0;
				continue;
			}

			double *angles = pdfs->angles[sensor][lh];
			vmask[sensor * NUM_LIGHTHOUSES + lh] = 1;

			meas[measCount++] = angles[0];
			meas[measCount++] = angles[1];
		}
	}
	return measCount;
}

static size_t construct_input_from_scene(SBAData *d, PoserDataLight *pdl, SurviveSensorActivations *scene, char *vmask,
										 double *meas, double *cov) {
	size_t rtn = 0;
	SurviveObject *so = d->opt.so;

	// fprintf(stderr, "#");

	for (size_t sensor = 0; sensor < so->sensor_ct; sensor++) {
		for (size_t lh = 0; lh < 2; lh++) {
			if (SurviveSensorActivations_isPairValid(scene, d->sensor_time_window, pdl->timecode, sensor, lh)) {
				const double *a = scene->angles[sensor][lh];
				// FLT a[2];
				// survive_apply_bsd_calibration(so->ctx, lh, _a, a);
				vmask[sensor * NUM_LIGHTHOUSES + lh] = 1;
				if (cov) {
					*(cov++) = d->sensor_variance +
							   abs((int32_t)pdl->timecode - (int32_t)scene->timecode[sensor][lh][0]) *
								   d->sensor_variance_per_second / (double)so->timebase_hz;
					*(cov++) = 0;
					*(cov++) = 0;
					*(cov++) = d->sensor_variance +
							   abs((int32_t)pdl->timecode - (int32_t)scene->timecode[sensor][lh][1]) *
								   d->sensor_variance_per_second / (double)so->timebase_hz;
				}
				meas[rtn++] = a[0];
				meas[rtn++] = a[1];
				// fprintf(stderr, "%.04f %.04f ", a[0], a[1]);
			} else {
				vmask[sensor * NUM_LIGHTHOUSES + lh] = 0;
				// fprintf(stderr, "%.06f %.06f ", sensor, lh, -2,-2);
			}
		}
	}
	// fprintf(stderr, "\n");
	return rtn;
}

void sba_set_cameras(SurviveObject *so, uint8_t lighthouse, SurvivePose *pose, SurvivePose *obj_pose, void *user) {
	sba_context *ctx = (sba_context *)user;
	ctx->camera_params[lighthouse] = *pose;
	if (obj_pose)
		ctx->obj_pose = *obj_pose;
	else
		ctx->obj_pose = LinmathPose_Identity;
}

static void str_metric_function(int j, int i, double *bi, double *xij, void *adata) {
	SurvivePose obj = *(SurvivePose *)bi;
	int sensor_idx = j >> 1;
	int lh = j & 1;

	sba_context *ctx = (sba_context *)(adata);
	SurviveObject *so = ctx->so;

	assert(lh < 2);
	assert(sensor_idx < so->sensor_ct);

	quatnormalize(obj.Rot, obj.Rot);

	// std::cerr << "Processing " << sensor_idx << ", " << lh << std::endl;
	SurvivePose *camera = &so->ctx->bsd[lh].Pose;
	survive_reproject_full(xij, &obj, &so->sensor_locations[sensor_idx * 3], camera, &so->ctx->bsd[lh],
						   &so->ctx->calibration_config);
}

static void str_metric_function_jac(int j, int i, double *bi, double *xij, void *adata) {
	SurvivePose obj = *(SurvivePose *)bi;
	int sensor_idx = j >> 1;
	int lh = j & 1;

	sba_context *ctx = (sba_context *)(adata);
	SurviveObject *so = ctx->so;

	assert(lh < 2);
	assert(sensor_idx < so->sensor_ct);

	quatnormalize(obj.Rot, obj.Rot);

	SurvivePose *camera = &so->ctx->bsd[lh].Pose;
	survive_reproject_full_jac_obj_pose(xij, &obj, &so->sensor_locations[sensor_idx * 3], camera, &so->ctx->bsd[lh],
										&so->ctx->calibration_config);
}

static double run_sba_find_3d_structure(SBAData *d, PoserDataLight *pdl, SurviveSensorActivations *scene,
										int max_iterations /* = 50*/, double max_reproj_error /* = 0.005*/,
										SurvivePose *out) {
	double *covx = 0;
	SurviveObject *so = d->opt.so;

	char *vmask = alloca(sizeof(char) * so->sensor_ct * NUM_LIGHTHOUSES);
	double *meas = alloca(sizeof(double) * 2 * so->sensor_ct * NUM_LIGHTHOUSES);
	double *cov = (d->sensor_variance_per_second > 0. && d->sensor_variance)
					  ? alloca(sizeof(double) * 2 * 2 * so->sensor_ct * NUM_LIGHTHOUSES)
					  : 0;
	size_t meas_size = construct_input_from_scene(d, pdl, scene, vmask, meas, cov);

	static int failure_count = 500;
	bool hasAllBSDs = true;
	for (int lh = 0; lh < so->ctx->activeLighthouses; lh++)
		hasAllBSDs &= so->ctx->bsd[lh].PositionSet;

	if (!hasAllBSDs || meas_size < d->required_meas) {
		if (hasAllBSDs && failure_count++ == 500) {
			SurviveContext *ctx = so->ctx;
			SV_INFO("Can't solve for position with just %u measurements", (unsigned int)meas_size);
			failure_count = 0;
		}
		if (meas_size < d->required_meas) {
			d->stats.meas_failures++;
		}
		return -1;
	}
	failure_count = 0;

	SurvivePose soLocation = {0};

	if (!general_optimizer_data_record_current_pose(&d->opt, &pdl->hdr, sizeof(*pdl), &soLocation)) {
		return -1;
	}

	double opts[SBA_OPTSSZ] = {0};
	double info[SBA_INFOSZ] = {0};

	sba_context ctx = {&pdl->hdr, so};

	opts[0] = SBA_INIT_MU;
	opts[1] = SBA_STOP_THRESH;
	opts[2] = SBA_STOP_THRESH;
	opts[3] = SBA_STOP_THRESH;
	opts[3] = SBA_STOP_THRESH; // max_reproj_error * meas.size();
	opts[4] = 0.0;

	int status = sba_str_levmar(1, // Number of 3d points
								0, // Number of 3d points to fix in spot
								NUM_LIGHTHOUSES * so->sensor_ct, vmask,
								soLocation.Pos, // Reads as the full pose though
								7,				// pnp -- SurvivePose
								meas,			// x* -- measurement data
								cov,			// cov data
								2,				// mnp -- 2 points per image
								str_metric_function,
								d->use_jacobian_function ? str_metric_function_jac : 0, // jacobia of metric_func
								&ctx,													// user data
								max_iterations,											// Max iterations
								0,														// verbosity
								opts,													// options
								info);													// info

	double rtn = -1;
	bool status_failure = status <= 0;
	bool error_failure = !general_optimizer_data_record_success(&d->opt, (info[1] / meas_size * 2));
	if (!status_failure && !error_failure) {
		quatnormalize(soLocation.Rot, soLocation.Rot);
		*out = soLocation;
		rtn = info[1] / meas_size * 2;
	} else {
		SurviveContext *ctx = so->ctx;
		// Docs say info[0] should be divided by meas; I don't buy it really...
		if (error_failure) {
			SV_INFO("%f original reproj error for %u meas", (info[0] / meas_size * 2), (int)meas_size);
			SV_INFO("%f cur reproj error", (info[1] / meas_size * 2));
		}
	}

	return rtn;
}

// Optimizes for LH position assuming object is posed at 0
static double run_sba(PoserDataFullScene *pdfs, SurviveObject *so, int max_iterations /* = 50*/,
					  double max_reproj_error /* = 0.005*/) {
	double *covx = 0;

	char *vmask = alloca(sizeof(char) * so->sensor_ct * NUM_LIGHTHOUSES);
	double *meas = alloca(sizeof(double) * 2 * so->sensor_ct * NUM_LIGHTHOUSES);
	size_t meas_size = construct_input(so, pdfs, vmask, meas);

	sba_context sbactx = {&pdfs->hdr, so, .camera_params = {so->ctx->bsd[0].Pose, so->ctx->bsd[1].Pose},
						  .obj_pose = so->OutPose};

	{
		const char *subposer = survive_configs(so->ctx, "seed-poser", SC_GET, "PoserEPNP");

		PoserCB driver = (PoserCB)GetDriver(subposer);
		SurviveContext *ctx = so->ctx;
		if (driver) {
			PoserData hdr = pdfs->hdr;
			memset(&pdfs->hdr, 0, sizeof(pdfs->hdr)); // Clear callback functions
			pdfs->hdr.pt = hdr.pt;
			pdfs->hdr.lighthouseposeproc = sba_set_cameras;
			pdfs->hdr.userdata = &sbactx;
			driver(so, &pdfs->hdr);
			pdfs->hdr = hdr;
		} else {
			SV_INFO("Not using a seed poser for SBA; results will likely be way off");
			for (int i = 0; i < 2; i++) {
				so->ctx->bsd[i].Pose = (SurvivePose){0};
				so->ctx->bsd[i].Pose.Rot[0] = 1.;
			}
		}
		// opencv_solver_poser_cb(so, (PoserData *)pdfs);
		// PoserCharlesSlow(so, (PoserData *)pdfs);
	}

	double opts[SBA_OPTSSZ] = {0};
	double info[SBA_INFOSZ] = {0};

	opts[0] = SBA_INIT_MU;
	opts[1] = SBA_STOP_THRESH;
	opts[2] = SBA_STOP_THRESH;
	opts[3] = SBA_STOP_THRESH;
	opts[3] = SBA_STOP_THRESH; // max_reproj_error * meas.size();
	opts[4] = 0.0;

	int status = sba_mot_levmar(so->sensor_ct,						  // number of 3d points
								so->ctx->activeLighthouses,			  // Number of cameras -- 2 lighthouses
								0,									  // Number of cameras to not modify
								vmask,								  // boolean vis mask
								(double *)&sbactx.camera_params[0],   // camera parameters
								sizeof(SurvivePose) / sizeof(double), // The number of floats that are in a camera param
								meas,								  // 2d points for 3d objs
								covx, // covariance of measurement. Null sets to identity
								2,	// 2 points per image
								metric_function,
								0,				// jacobia of metric_func
								&sbactx,		// user data
								max_iterations, // Max iterations
								0,				// verbosity
								opts,			// options
								info);			// info

	if (status >= 0) {
		SurvivePose additionalTx = {0};
		for (int i = 0; i < so->ctx->activeLighthouses; i++) {
			if (quatmagnitude(sbactx.camera_params[i].Rot) != 0) {
				PoserData_lighthouse_pose_func(&pdfs->hdr, so, i, &additionalTx, &sbactx.camera_params[i],
											   &sbactx.obj_pose);
			}
		}
	} else {
		SurviveContext *ctx = so->ctx;
		SV_INFO("SBA was unable to run %d", status);
	}
	// Docs say info[0] should be divided by meas; I don't buy it really...
	// std::cerr << info[0] / meas.size() * 2 << " original reproj error" << std::endl;

	{
		SurviveContext *ctx = so->ctx;
		// Docs say info[0] should be divided by meas; I don't buy it really...
		SV_INFO("%f original reproj error for %u meas", (info[0] / meas_size * 2), (int)meas_size);
		SV_INFO("%f cur reproj error", (info[1] / meas_size * 2));
	}

	return info[1] / meas_size * 2;
}

int PoserSBA(SurviveObject *so, PoserData *pd) {
	SurviveContext *ctx = so->ctx;
	if (so->PoserData == 0) {
		so->PoserData = calloc(1, sizeof(SBAData));
		SBAData *d = so->PoserData;

		general_optimizer_data_init(&d->opt, so);
		d->useIMU = survive_configi(ctx, "sba-use-imu", SC_GET, 1);
		d->required_meas = survive_configi(ctx, "sba-required-meas", SC_GET, 8);

		d->sensor_time_window =
			survive_configi(ctx, "sba-time-window", SC_GET, SurviveSensorActivations_default_tolerance * 2);
		d->sensor_variance_per_second = survive_configf(ctx, "sba-sensor-variance-per-sec", SC_GET, 10.0);
		d->sensor_variance = survive_configf(ctx, "sba-sensor-variance", SC_GET, 1.0);
		d->use_jacobian_function = survive_configi(ctx, "sba-use-jacobian-function", SC_GET, 1.0);

		SV_INFO("Initializing SBA:");
		SV_INFO("\tsba-required-meas: %d", d->required_meas);
		SV_INFO("\tsba-sensor-variance: %f", d->sensor_variance);
		SV_INFO("\tsba-sensor-variance-per-sec: %f", d->sensor_variance_per_second);
		SV_INFO("\tsba-time-window: %d", d->sensor_time_window);
		SV_INFO("\tsba-use-imu: %d", d->useIMU);
		SV_INFO("\tsba-use-jacobian-function: %d", d->use_jacobian_function);
	}
	SBAData *d = so->PoserData;
	switch (pd->pt) {
	case POSERDATA_LIGHT: {
		// No poses if calibration is ongoing
		if (ctx->calptr && ctx->calptr->stage < 5)
			return 0;
		SurviveSensorActivations *scene = &so->activations;
		PoserDataLight *lightData = (PoserDataLight *)pd;
		SurvivePose estimate;

		// only process sweeps
		FLT error = -1;
		if (d->last_lh != lightData->lh || d->last_acode != lightData->acode) {
			error = run_sba_find_3d_structure(d, lightData, scene, 100, .5, &estimate);

			d->last_lh = lightData->lh;
			d->last_acode = lightData->acode;

			if (error < 0) {

			} else {
				quatnormalize(estimate.Rot, estimate.Rot);

				if (d->useIMU) {
					FLT var_meters = 0.5;
					FLT var_quat = error + .05;
					FLT var[7] = {error * var_meters, error * var_meters, error * var_meters, error * var_quat,
								  error * var_quat,   error * var_quat,   error * var_quat};

					survive_imu_tracker_integrate_observation(so, lightData->timecode, &d->tracker, &estimate, var);
					estimate = d->tracker.pose;
				}

				LinmathVec3d pvar = {.1, .1, .1};
				FLT rvar = .01;
				//survive_kpose_integrate_pose(&d->kpose, lightData->timecode, &estimate, pvar, rvar);
				//estimate = d->kpose.state.pose;

				PoserData_poser_pose_func(&lightData->hdr, so, &estimate);
			}
		}
		return 0;
	}
	case POSERDATA_FULL_SCENE: {
		SurviveContext *ctx = so->ctx;
		PoserDataFullScene *pdfs = (PoserDataFullScene *)(pd);
		double error = run_sba(pdfs, so, 100, .005);
		// std::cerr << "Average reproj error: " << error << std::endl;
		return 0;
	}
	case POSERDATA_DISASSOCIATE: {
		SV_INFO("SBA stats:");
		SV_INFO("\tmeas failures %d", d->stats.meas_failures);
		general_optimizer_data_dtor(&d->opt);
		free(d);
		so->PoserData = 0;
		return 0;
	}
	case POSERDATA_IMU: {

	  PoserDataIMU * imu = (PoserDataIMU*)pd;
	  if (ctx->calptr && ctx->calptr->stage < 5) {
	  } else if (d->useIMU) {
		  survive_imu_tracker_integrate(so, &d->tracker, imu);
		  PoserData_poser_pose_func(pd, so, &d->tracker.pose);
	  }

	  general_optimizer_data_record_imu(&d->opt, imu);
	}
	}
	return -1;
}

REGISTER_LINKTIME(PoserSBA);
