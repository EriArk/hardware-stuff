'use strict';

// Electron SerialPort IDs are decimal strings; Web Serial filters use numbers.
// Select only an unambiguous supported reader, then verify firmware over USB.
function selectReaderPort(ports) {
 const matches=ports.filter(p=>p.vendorId==='12346'&&p.productId==='4097');
 return matches.length===1?matches[0].portId:'';
}
module.exports={selectReaderPort};
