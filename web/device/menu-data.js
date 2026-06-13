// The on-device launcher menu tree: categories -> apps, with per-app context actions.
// Icons are single glyphs (the firmware draws one glyph in a colored circle, so we keep
// to one visual unit per icon). Colors are the per-category accent (also used in firmware
// as RGB565). Every node carries a short, clear English `desc` for the instruction line.

const ACTIONS = [
  { id: 'open', label: 'Open',        icon: '▶', desc: 'Launch this app full-screen' },
  { id: 'pin',  label: 'Pin to Home', icon: '★', desc: 'Show this app at the top of Home' },
  { id: 'info', label: 'App Info',    icon: 'ⓘ', desc: 'Version, permissions and storage use' },
];

export const ROOT = {
  id: 'home', type: 'menu', label: 'Home', icon: '⌂', color: '#4ea1ff',
  desc: 'NucleoOS home',
  items: [
    {
      id: 'media', type: 'menu', label: 'Media', icon: '♪', color: '#ff7eb6',
      desc: 'Audio, photos and recordings',
      items: [
        { id: 'recorder', type: 'app', label: 'Voice Recorder', icon: '●', color: '#ff5a5a',
          desc: 'Record the mic to a WAV on the SD card', actions: ACTIONS },
        { id: 'music', type: 'app', label: 'Music', icon: '♫', color: '#ff7eb6',
          desc: 'Play MP3/WAV through the speaker', actions: ACTIONS },
        { id: 'photos', type: 'app', label: 'Photos', icon: '▦', color: '#ffd166',
          desc: 'Browse images on the SD card', actions: ACTIONS },
        { id: 'video-player', type: 'app', label: 'Video', icon: '►', color: '#ff7eb6',
          desc: 'Play video on a connected screen', actions: ACTIONS },
      ],
    },
    {
      id: 'tools', type: 'menu', label: 'Tools', icon: '⚙', color: '#7CFC9A',
      desc: 'Everyday utilities',
      items: [
        { id: 'calculator', type: 'app', label: 'Calculator', icon: '=', color: '#7CFC9A',
          desc: 'Type sums on the keyboard', actions: ACTIONS },
        { id: 'clock', type: 'app', label: 'Clock', icon: '⏱', color: '#4ea1ff',
          desc: 'Time, date and stopwatch', actions: ACTIONS },
        { id: 'files', type: 'app', label: 'Files', icon: '▤', color: '#ffd166',
          desc: 'Browse and open SD card files', actions: ACTIONS },
        { id: 'notepad', type: 'app', label: 'Notes', icon: '✎', color: '#4ea1ff',
          desc: 'Read and edit text files', actions: ACTIONS },
        { id: 'ir', type: 'app', label: 'IR Remote', icon: '➤', color: '#c792ea',
          desc: 'Send infrared remote codes', actions: ACTIONS },
        { id: 'dosbox', type: 'app', label: 'DOS Box', icon: '#', color: '#ffd166',
          desc: 'Play classic DOS games (in the browser)', actions: ACTIONS },
        { id: 'automation-studio', type: 'app', label: 'Automation', icon: '∞', color: '#7CFC9A',
          desc: 'Build automations and macros', actions: ACTIONS },
      ],
    },
    {
      id: 'system', type: 'menu', label: 'System', icon: '◆', color: '#4ea1ff',
      desc: 'Device status and configuration',
      items: [
        { id: 'info', type: 'app', label: 'Connection', icon: 'ⓘ', color: '#4ea1ff',
          desc: 'Wi-Fi, IP and web address to reach this device', actions: ACTIONS },
        { id: 'status', type: 'app', label: 'System Status', icon: '◉', color: '#4ea1ff',
          desc: 'Battery, RAM, storage and network', actions: ACTIONS },
        { id: 'network', type: 'app', label: 'Network', icon: '≋', color: '#7CFC9A',
          desc: 'Wi-Fi join or access-point mode', actions: ACTIONS },
        { id: 'settings', type: 'app', label: 'Settings', icon: '⚙', color: '#8aa0c8',
          desc: 'Theme, brightness, device name', actions: ACTIONS },
        { id: 'remote', type: 'app', label: 'Remote Control', icon: '⇄', color: '#4ea1ff',
          desc: 'Hand the device to a web client when one connects', actions: ACTIONS },
        { id: 'log-viewer', type: 'app', label: 'Logs', icon: '☰', color: '#8aa0c8',
          desc: 'View the live system log', actions: ACTIONS },
      ],
    },
    {
      id: 'connect', type: 'menu', label: 'Connect', icon: '⇄', color: '#c792ea',
      desc: 'Use NucleoOS from a PC or phone',
      items: [
        { id: 'companion', type: 'app', label: 'Companion App', icon: '⇄', color: '#c792ea',
          desc: 'Download NucleoConnect for your PC', actions: ACTIONS },
        { id: 'swarm', type: 'app', label: 'Swarm', icon: '⁂', color: '#ff7eb6',
          desc: 'Share clipboard across nearby devices', actions: ACTIONS },
      ],
    },
  ],
};
