# Run inside the isolated Terminus web container after uploading the first PNG.
require "hanami/boot"
require "json"

app = Hanami.app
palettes = app["repositories.palette"]
models = app["repositories.model"]
devices = app["repositories.device"]
playlists = app["repositories.playlist"]
items = app["repositories.playlist_item"]

palette = palettes.find_by(name: "kobo_gray16") || palettes.create(
  name: "kobo_gray16", label: "Kobo 16 grayscale levels", kind: "terminus", grays: 16
)
model = models.find_by(name: "kobo_touch_a") || models.create(
  name: "kobo_touch_a", label: "Kobo Touch A / N905", kind: "terminus",
  description: "Verified Freescale Touch A, 600x800 portrait, FBInk client",
  width: 600, height: 800, rotation: 0, offset_x: 0, offset_y: 0,
  scale_factor: 1, mime_type: "image/png", default_palette_id: palette.id
)
mac = ENV.fetch("KOBO_MAC")
device = devices.find_by(mac_address: mac)
unless device
  attributes = app["aspects.devices.defaulter"].call.merge(
    model_id: model.id, mac_address: mac, label: "Kobo dashboard",
    firmware_update: false, firmware_version: "1.6.10", refresh_rate: 900
  )
  device = devices.create(**attributes)
end
result = app["aspects.screens.upserter"].call(
  model_id: model.id, device_id: device.id, label: "First Kobo dashboard",
  name: "kobo_first_dashboard", uri: "/tmp/kobo-first.png", mode: "dither", preprocessed: true
)
screen = result.value!
playlist = playlists.find_by(name: "kobo_dashboard") || playlists.create(
  name: "kobo_dashboard", label: "Kobo dashboard"
)
item = items.find_by(playlist_id: playlist.id, screen_id: screen.id) || items.create_with_position(
  playlist_id: playlist.id, screen_id: screen.id
)
playlists.update playlist.id, current_item_id: item.id
devices.update device.id, playlist_id: playlist.id, firmware_update: false
receipt = {device_id: device.id, api_key: device.api_key, mac_address: mac,
           model_id: model.id, playlist_id: playlist.id, screen_id: screen.id}
File.open("/tmp/kobo-device.json", "w", 0o600) { |file| file.write JSON.pretty_generate(receipt) }
puts JSON.generate(status: "provisioned", device_id: device.id, width: 600, height: 800)
