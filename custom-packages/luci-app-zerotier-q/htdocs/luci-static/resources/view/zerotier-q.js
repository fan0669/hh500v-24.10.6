'use strict';
'require view';
'require fs';
'require ui';

var CLI = '/usr/bin/zerotier-cli';

function call(args) {
	return fs.exec(CLI, args).then(function(res) {
		return { code: res.code, out: (res.stdout || '').trim(), err: (res.stderr || '').trim() };
	}).catch(function(e) {
		return { code: -1, out: '', err: String(e) };
	});
}

function parseNetworks(out) {
	var list = [];
	(out || '').split('\n').forEach(function(line) {
		line = line.trim();
		/* <ztaddr> <path> <name> <id> ... */
		var m = line.match(/^[0-9a-f]{10}\s+\S+\s+(\S+)\s+([0-9a-f]{16})/i);
		if (m)
			list.push({ name: m[1], id: m[2], raw: line });
	});
	return list;
}

return view.extend({
	load: function() {
		return Promise.all([ call(['status']), call(['listnetworks']) ]);
	},

	handleJoin: function(id) {
		var self = this;
		return call(['join', id]).then(function(r) {
			ui.addNotification(null, E('p', r.code === 0 ? _('Join request sent: %s').format(id) : (r.err || r.out || _('Failed'))));
			return self.load();
		});
	},

	handleLeave: function(id) {
		var self = this;
		return call(['leave', id]).then(function(r) {
			ui.addNotification(null, E('p', r.code === 0 ? _('Left network: %s').format(id) : (r.err || r.out || _('Failed'))));
			return self.load();
		});
	},

	render: function(data) {
		var status = data[0], nets = data[1];
		var networks = parseNetworks(nets.out);

		var table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Network')),
				E('th', { 'class': 'th' }, _('Network ID')),
				E('th', { 'class': 'th' }, _('Action'))
			])
		]);

		if (!networks.length) {
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'colspan': 3 }, _('No networks joined yet.'))
			]));
		} else {
			networks.forEach(function(n) {
				table.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, n.name),
					E('td', { 'class': 'td' }, n.id),
					E('td', { 'class': 'td' }, [
						E('button', {
							'class': 'btn cbi-button cbi-button-remove',
							'click': ui.createHandlerFn(this, function() { return this.handleLeave(n.id); })
						}, _('Leave'))
					])
				]));
			}, this);
		}

		var input = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'placeholder': 'e.g. 8056c2e21c000001', 'style': 'min-width:22em' });

		var joinBtn = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(this, function() {
				var v = (input.value || '').trim();
				if (!/^[0-9a-f]{16}$/i.test(v)) {
					ui.addNotification(null, E('p', _('Network ID must be 16 hex characters.')));
					return;
				}
				return this.handleJoin(v);
			})
		}, _('Join'));

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('ZeroTier')),
			E('div', { 'class': 'cbi-map-descr' }, _('Control ZeroTier via zerotier-cli. Node must be started by the zerotier service.')),

			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Status')),
				E('pre', { 'style': 'white-space:pre-wrap' }, status.out || status.err || _('no output'))
			]),

			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Joined networks')),
				table
			]),

			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Join a network')),
				E('div', { 'class': 'cbi-value' }, [ input, ' ', joinBtn ])
			]),

			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, function() { return location.reload(); })
				}, _('Refresh'))
			])
		]);
	},

	handleSaveApply: null,
	handleSave: null
});
