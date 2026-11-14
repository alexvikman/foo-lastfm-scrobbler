# Security policy

## Supported versions

Only the latest release receives security fixes.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub: open the repository's
**Security** tab and choose **Report a vulnerability**. Do not open a public
issue for a security problem.

Include the component version, the foobar2000 version, and the steps needed to
reproduce the problem. Never include a Last.fm session key, password, or the
contents of your foobar2000 configuration in a report.

## Known limitations

These follow from how Last.fm desktop authentication and Windows DPAPI work and
are not treated as vulnerabilities:

- The application API key and shared secret are compiled into the component
  and can be read from it. They identify the application, not a user, and do
  not grant access to an account without that user's approval on Last.fm.
- The stored session and scrobble queue are encrypted with DPAPI for the
  current Windows user. Other programs running as the same user can decrypt
  them.
- Last.fm requires MD5 request signatures.
