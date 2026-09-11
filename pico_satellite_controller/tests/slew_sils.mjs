import assert from "node:assert/strict";

const config = {
  angleGain: 0.70,
  angleIntegralGain: 0.10,
  maxIntegralRate: 2.0,
  integralZone: 12.0,
  rateGain: 1.50,
  wheelCommandGain: 3.00,
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
  let holdStartedAt = null;

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
    const bodyAccelerationRequest =
      config.rateGain * (rateReference - bodyRate);
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
    const bodyAcceleration =
      -inertiaRatio * wheelAcceleration - viscousDrag * bodyRate -
      restoringStiffness * wrapError(bodyAngle - equilibriumAngle);
    bodyRate += bodyAcceleration * dt;
    bodyAngle += bodyRate * dt;

    if (Math.abs(error) <= config.settleAngle &&
        Math.abs(bodyRate) <= config.settleRate) {
      settled += dt;
    } else {
      settled = 0;
    }
    if (holdStartedAt === null && settled >= config.settleTime) {
      holdStartedAt = time;
      if (holdDuration === 0) {
        return { result: "HOLD", time, bodyAngle, bodyRate, wheelCommand };
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
      };
    }

    const pushingLimit =
      (requestedWheelCommand > config.maxWheelCommand && wheelDelta > 0) ||
      (requestedWheelCommand < -config.maxWheelCommand && wheelDelta < 0);
    saturated = pushingLimit ? saturated + dt : 0;
    if (saturated >= config.saturationTimeout) {
      return { result: "SATURATION", time, bodyAngle, bodyRate, wheelCommand };
    }
  }
  return { result: "TIMEOUT", bodyAngle, bodyRate, wheelCommand };
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

assert.equal(wrapError(180), 180);
assert.equal(wrapError(-180), -180);
passed += 2;

console.log(`PASS: ${passed} slew SILS assertions`);
