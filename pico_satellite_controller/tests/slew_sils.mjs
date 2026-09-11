import assert from "node:assert/strict";

const config = {
  angleGain: 0.70,
  angleIntegralGain: 0.10,
  maxIntegralRate: 2.0,
  integralZone: 12.0,
  rateGain: 1.50,
  wheelCommandGain: 3.00,
  breakawayMinAccel: 8.0,
  breakawayRateThreshold: 0.20,
  breakawayAngleThreshold: 1.0,
  breakawayDelay: 0.20,
  maxBodyRate: 5.0,
  maxWheelCommand: 70.0,
  settleAngle: 3.0,
  settleRate: 0.8,
  settleTime: 1.0,
  timeout: 90.0,
  saturationTimeout: 1.5,
};

function clamp(value, minimum, maximum) {
  return Math.max(minimum, Math.min(maximum, value));
}

function wrapError(error) {
  const unwrapped = error;
  error = ((error + 180) % 360 + 360) % 360 - 180;
  return error === -180 && unwrapped > 0 ? 180 : error;
}

function angleError(target, current, relative) {
  const error = target - current;
  return relative ? error : wrapError(error);
}

/*
 * One-axis SILS plant:
 *   (Jb + Jw) * body_accel + Jw * wheel_accel = -b * body_rate
 *
 * inertiaRatio is Jw / (Jb + Jw).  The FS90R is represented by a
 * first-order speed response whose 100% command is 780 deg/s (130 rpm).
 * This is deliberately a bounded family test, not a claim that these are
 * the measured parameters of the physical seminar hardware.
 */
function simulate({
  target,
  inertiaRatio,
  viscousDrag,
  restoringStiffness = 0,
  equilibriumAngle = 0,
  holdDuration = 0,
  staticFriction = 0,
  breakawayEnabled = true,
}) {
  const dt = 0.02;
  const servoTimeConstant = 0.30;
  const fullWheelSpeed = 780.0;
  let bodyAngle = 0;
  let bodyRate = 0;
  let wheelSpeed = 0;
  let wheelCommand = 0;
  let integralRate = 0;
  let settled = 0;
  let saturated = 0;
  let stalled = 0;
  let holdStartedAt = null;
  let firstMotionAt = null;

  for (let time = 0; time <= config.timeout; time += dt) {
    const error = wrapError(target - bodyAngle);
    if (Math.abs(error) <= config.integralZone &&
        Math.abs(wheelCommand) < 0.90 * config.maxWheelCommand) {
      integralRate = clamp(
        integralRate + config.angleIntegralGain * error * dt,
        -config.maxIntegralRate,
        config.maxIntegralRate,
      );
    }
    const rateReference = clamp(
      config.angleGain * error + integralRate,
      -config.maxBodyRate,
      config.maxBodyRate,
    );
    let bodyAccelerationRequest =
      config.rateGain * (rateReference - bodyRate);
    const rotationStillRequired =
      Math.abs(error) > config.breakawayAngleThreshold;
    const bodyIsStationary =
      Math.abs(bodyRate) <= config.breakawayRateThreshold;
    if (breakawayEnabled && rotationStillRequired && bodyIsStationary) {
      stalled = Math.min(config.breakawayDelay, stalled + dt);
    } else {
      stalled = 0;
    }
    const breakawayActive = breakawayEnabled && rotationStillRequired &&
      bodyIsStationary && stalled >= config.breakawayDelay;
    if (breakawayActive) {
      const direction = error >= 0 ? 1 : -1;
      if (direction * bodyAccelerationRequest < config.breakawayMinAccel) {
        bodyAccelerationRequest = direction * config.breakawayMinAccel;
      }
    }
    const wheelDelta =
      -config.wheelCommandGain * bodyAccelerationRequest * dt;
    const requestedWheelCommand = wheelCommand + wheelDelta;
    wheelCommand = clamp(
      requestedWheelCommand,
      -config.maxWheelCommand,
      config.maxWheelCommand,
    );

    const previousWheelSpeed = wheelSpeed;
    wheelSpeed +=
      (wheelCommand / 100 * fullWheelSpeed - wheelSpeed) *
      dt / servoTimeConstant;
    const wheelAcceleration = (wheelSpeed - previousWheelSpeed) / dt;
    let bodyAcceleration =
      -inertiaRatio * wheelAcceleration - viscousDrag * bodyRate -
      restoringStiffness * wrapError(bodyAngle - equilibriumAngle);
    if (staticFriction > 0) {
      if (Math.abs(bodyRate) < 0.001 &&
          Math.abs(bodyAcceleration) <= staticFriction) {
        bodyAcceleration = 0;
        bodyRate = 0;
      } else {
        const motionDirection = Math.abs(bodyRate) >= 0.001
          ? Math.sign(bodyRate) : Math.sign(bodyAcceleration);
        bodyAcceleration -= motionDirection * 0.75 * staticFriction;
      }
    }
    bodyRate += bodyAcceleration * dt;
    bodyAngle += bodyRate * dt;
    if (firstMotionAt === null &&
        (Math.abs(bodyAngle) > 0.01 || Math.abs(bodyRate) > 0.05)) {
      firstMotionAt = time;
    }

    if (Math.abs(error) <= config.settleAngle &&
        Math.abs(bodyRate) <= config.settleRate) {
      settled += dt;
    } else {
      settled = 0;
    }
    if (holdStartedAt === null && settled >= config.settleTime) {
      holdStartedAt = time;
      if (holdDuration === 0) {
        return {
          result: "HOLD", time, bodyAngle, bodyRate, wheelCommand,
          firstMotionAt,
        };
      }
    }
    if (holdStartedAt !== null && time - holdStartedAt >= holdDuration) {
      return {
        result: "HELD",
        time,
        bodyAngle,
        bodyRate,
        wheelCommand,
        integralRate,
        firstMotionAt,
      };
    }

    const pushingLimit =
      (requestedWheelCommand > config.maxWheelCommand && wheelDelta > 0) ||
      (requestedWheelCommand < -config.maxWheelCommand && wheelDelta < 0);
    saturated = pushingLimit ? saturated + dt : 0;
    if (saturated >= config.saturationTimeout) {
      return {
        result: "SATURATION", time, bodyAngle, bodyRate, wheelCommand,
        firstMotionAt,
      };
    }
  }
  return {
    result: "TIMEOUT", bodyAngle, bodyRate, wheelCommand, firstMotionAt,
  };
}

let passed = 0;
for (const target of [-90, -30, 0, 30, 90]) {
  for (const inertiaRatio of [0.02, 0.03, 0.05]) {
    for (const viscousDrag of [0, 0.02, 0.05]) {
      const result = simulate({ target, inertiaRatio, viscousDrag });
      assert.equal(result.result, "HOLD",
        `expected HOLD: ${JSON.stringify({ target, inertiaRatio, viscousDrag, result })}`);
      assert.ok(Math.abs(wrapError(target - result.bodyAngle)) <= config.settleAngle);
      assert.ok(Math.abs(result.bodyRate) <= config.settleRate);
      passed += 1;
    }
  }
}

const weakActuator = simulate({
  target: 90,
  inertiaRatio: 0.003,
  viscousDrag: 0.05,
});
assert.equal(weakActuator.result, "SATURATION");
passed += 1;

// A repeatable return-to-zero torque must be rejected after target capture.
// This verifies that HOLD keeps controlling and that its slow integral bias
// learns the sustained wheel acceleration instead of stopping the servo.
const tiltedSuspension = simulate({
  target: 30,
  inertiaRatio: 0.03,
  viscousDrag: 0.02,
  restoringStiffness: 0.002,
  equilibriumAngle: 0,
  holdDuration: 15,
});
assert.equal(tiltedSuspension.result, "HELD");
assert.ok(Math.abs(wrapError(30 - tiltedSuspension.bodyAngle)) <= 1.0,
  JSON.stringify(tiltedSuspension));
passed += 2;

// A small slew must break static friction promptly. Without the boost the
// ordinary PI/rate loop takes substantially longer to build enough wheel
// acceleration for the same plant.
const smallStictionSlew = simulate({
  target: 2,
  inertiaRatio: 0.03,
  viscousDrag: 0.02,
  staticFriction: 4.0,
});
const smallStictionSlewWithoutBoost = simulate({
  target: 2,
  inertiaRatio: 0.03,
  viscousDrag: 0.02,
  staticFriction: 4.0,
  breakawayEnabled: false,
});
assert.notEqual(smallStictionSlew.firstMotionAt, null,
  JSON.stringify(smallStictionSlew));
assert.equal(smallStictionSlew.result, "HOLD",
  JSON.stringify(smallStictionSlew));
assert.ok(smallStictionSlew.firstMotionAt < 1.0,
  JSON.stringify(smallStictionSlew));
assert.ok(smallStictionSlewWithoutBoost.firstMotionAt === null ||
  smallStictionSlewWithoutBoost.firstMotionAt > smallStictionSlew.firstMotionAt,
  JSON.stringify({ smallStictionSlew, smallStictionSlewWithoutBoost }));
passed += 4;

assert.equal(wrapError(180), 180);
assert.equal(wrapError(-180), -180);
assert.equal(angleError(360, 0, true), 360);
assert.equal(angleError(-360, 0, true), -360);
assert.equal(angleError(360, 0, false), 0);
passed += 5;

console.log(`PASS: ${passed} slew SILS assertions`);
