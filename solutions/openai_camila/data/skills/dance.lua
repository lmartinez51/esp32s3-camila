-- /littlefs/skills/dance.lua
-- Autonomous dance routine for ELEGOO BT16 / Robot

-- 1. Warm-up & Center Head
robot.set_servo(90)
robot.sleep_ms(300)

-- 2. Head Shake & Wheel Wiggle (Beat 1)
for i = 1, 2 do
    robot.set_servo(45)
    robot.move("LEFT", 150, 80)
    robot.sleep_ms(250)

    robot.set_servo(135)
    robot.move("RIGHT", 150, 80)
    robot.sleep_ms(250)
end

-- 3. Center & Step Forward/Backward (Beat 2)
robot.set_servo(90)
robot.sleep_ms(200)
robot.move("FORWARD", 250, 70)
robot.sleep_ms(350)
robot.move("BACKWARD", 250, 70)
robot.sleep_ms(350)

-- 4. Spin Move
robot.move("RIGHT", 400, 100)
robot.sleep_ms(500)

-- 5. Victory Bow & Voice Notification
robot.set_servo(90)
robot.sleep_ms(200)
camila.notify("¡Terminé mi bailecito! ¿A poco no me salió con estilo?")
