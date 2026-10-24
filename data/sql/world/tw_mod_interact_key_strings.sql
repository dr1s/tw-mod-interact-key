-- tw-mod-interact-key module strings
-- id 0 is reserved by the core.

DELETE FROM `module_string` WHERE `module` = 'tw-mod-interact-key';
INSERT INTO `module_string` (`module`, `id`, `content_default`) VALUES
('tw-mod-interact-key', 1, 'The interact command is disabled on this server.'),
('tw-mod-interact-key', 2, 'Nothing to interact with within %.1f yards.'),
('tw-mod-interact-key', 3, 'Interacting with %s: %s (%.1fy)');

-- Add locale translations below as needed. The core falls back to
-- content_default when a locale-specific row is missing.
-- Example:
-- DELETE FROM `module_string_locale` WHERE `module` = 'tw-mod-interact-key';
-- INSERT INTO `module_string_locale` (`module`, `id`, `locale`, `content`) VALUES
-- ('tw-mod-interact-key', 1, 3, 'Der Interaktionsbefehl ist auf diesem Server deaktiviert.');
