const express = require("express");
const app = express();
const PORT = 3000;

app.use(express.json());

app.get('/', (req, res) => {
  res.send('Hello, World!');
});


app.get('/about', (req, res) => {
  res.status(200).send({
    message: 'This is the about page.',
    status: 'success'
  });
});


app.listen(PORT, () => {
  console.log(`Server listening on port ${PORT}`);
});