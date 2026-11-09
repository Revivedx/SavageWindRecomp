"""The spike must fall back as a group if even one pattern fails on a known build."""
import game_sites as gs

class MissingPattern:
    def locate(self, site):
        return (None, 0) if site.name == gs.OPT_TWO_SITES[-1] else (gs.KNOWN['1.9.0'].get(site.name), 1)

found = gs.resolve(MissingPattern(), gs.KNOWN['1.9.0'])
assert not set(gs.OPT_TWO_SITES) & found.keys(), 'a KNOWN address must not enable an unverified spike'
assert found['opt_decide'] == gs.KNOWN['1.9.0']['opt_decide'], 'existing Options handling survives'
print('two-button sites: one failed pattern disables the whole spike PASS')

# Guard every site and both failure modes, including a unique match at a wrong address.
for missing in gs.OPT_TWO_SITES:
    for wrong, hits in ((None, 0), (None, 2), (gs.KNOWN['1.9.0'][missing] + 4, 1)):
        class OneBadSite:
            def locate(self, site):
                return (wrong, hits) if site.name == missing else (gs.KNOWN['1.9.0'].get(site.name), 1)
        assert not set(gs.OPT_TWO_SITES) & gs.resolve(OneBadSite(), gs.KNOWN['1.9.0']).keys()
print('all spike sites: missing, ambiguous and mismatched signatures fall back PASS')
