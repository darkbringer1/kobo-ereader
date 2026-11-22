# Update the existing device's playlist screen without changing its access token.
require "hanami/boot"
require "json"

app = Hanami.app
device = app["repositories.device"].find(Integer(ENV.fetch("KOBO_DEVICE_ID")))
abort "Provision the Kobo device first" unless device
abort "Refusing a device with firmware updates enabled" if device.firmware_update
screen = app["aspects.screens.upserter"].call(
  model_id: device.model_id, device_id: device.id,
  label: "Berlin / Weather and Daily Dashboard", name: "kobo_first_dashboard",
  uri: "/tmp/kobo-dashboard.png", mode: "dither", preprocessed: true
).value!
puts JSON.generate(status: "published", device_id: device.id, screen_id: screen.id)
